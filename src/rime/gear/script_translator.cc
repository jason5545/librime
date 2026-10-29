//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Script translator
//
// 2011-07-10 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <stack>
#include <cmath>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/language.h>
#include <fstream>
#include <rime/deployer.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/switches.h>
#include <rime/translation.h>
#include <limits>
#include <rime/algo/syllabifier.h>
#include <rime/dict/corrector.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/prism.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/english_lexicon.h>
#include <rime/gear/poet.h>
#include <rime/gear/script_translator.h>
#include <rime/gear/translator_commons.h>

// static const char* quote_left = "\xef\xbc\x88";
// static const char* quote_right = "\xef\xbc\x89";

namespace rime {

// 蓬萊（speller/digit_lead_tones）：跟 abc_segmentor 同一個標籤
static const char kDigitLeadTag[] = "digit_lead";

namespace {

struct SyllabifyTask {
  const Code& code;
  const SyllableGraph& graph;
  size_t target_pos;
  function<void(SyllabifyTask* task,
                size_t depth,
                size_t current_pos,
                size_t next_pos)>
      push;
  function<void(SyllabifyTask* task, size_t depth)> pop;
};

static bool syllabify_dfs(SyllabifyTask* task,
                          size_t depth,
                          size_t current_pos) {
  if (depth == task->code.size()) {
    return current_pos == task->target_pos;
  }
  SyllableId syllable_id = task->code.at(depth);
  auto z = task->graph.edges.find(current_pos);
  if (z == task->graph.edges.end())
    return false;
  // favor longer spellings
  for (const auto& y : boost::adaptors::reverse(z->second)) {
    size_t end_vertex_pos = y.first;
    if (end_vertex_pos > task->target_pos)
      continue;
    auto x = y.second.find(syllable_id);
    if (x != y.second.end()) {
      task->push(task, depth, current_pos, end_vertex_pos);
      if (syllabify_dfs(task, depth + 1, end_vertex_pos))
        return true;
      task->pop(task, depth);
    }
  }
  return false;
}

}  // anonymous namespace

class ScriptSyllabifier : public PhraseSyllabifier {
 public:
  ScriptSyllabifier(ScriptTranslator* translator,
                    Corrector* corrector,
                    const string& input,
                    size_t start)
      : translator_(translator),
        input_(input),
        start_(start),
        syllabifier_(translator->delimiters(),
                     translator->enable_completion(),
                     translator->strict_spelling()) {
    if (corrector) {
      syllabifier_.EnableCorrection(corrector);
    }
  }

  virtual Spans Syllabify(const Phrase* phrase);
  size_t BuildSyllableGraph(Prism& prism);
  string GetPreeditString(const Phrase& cand) const;
  string GetOriginalSpelling(const Phrase& cand) const;
  bool IsCorrection(const Code& code, size_t code_length) const;

  const SyllableGraph& syllable_graph() const { return syllable_graph_; }

 protected:
  ScriptTranslator* translator_;
  string input_;
  size_t start_;
  Syllabifier syllabifier_;
  SyllableGraph syllable_graph_;
};

class ScriptTranslation : public Translation {
 public:
  ScriptTranslation(ScriptTranslator* translator,
                    Corrector* corrector,
                    Poet* poet,
                    const string& input,
                    size_t start,
                    size_t end_of_input,
                    int max_sentences,
                    double sentence_cutoff_threshold)
      : translator_(translator),
        poet_(poet),
        input_(input),
        start_(start),
        end_of_input_(end_of_input),
        syllabifier_(
            New<ScriptSyllabifier>(translator, corrector, input, start)),
        corrector_(corrector),
        enable_correction_(corrector),
        max_sentences_(max_sentences),
        sentence_cutoff_threshold_(sentence_cutoff_threshold) {
    set_exhausted(true);
  }
  bool Evaluate(Dictionary* dict, UserDictionary* user_dict);
  virtual bool Next();
  virtual an<Candidate> Peek();

 protected:
  bool CheckEmpty();
  bool IsNormalSpelling() const;
  bool PrepareCandidate();
  template <class QueryResult>
  void EnrollEntries(map<int, DictEntryList>& entries_by_end_pos,
                     const an<QueryResult>& query_result);
  WordGraph PrepareForMakingSentence(Dictionary* dict,
                                     UserDictionary* user_dict);
  an<Sentence> MakeSentence(Dictionary* dict, UserDictionary* user_dict);
  deque<an<Sentence>> MakeSentences(Dictionary* dict,
                                    UserDictionary* user_dict);
  // 蓬萊：英文單字進整句
  void MakeEnglishCandidates(Dictionary* dict, UserDictionary* user_dict);
  bool EnrollSegment(Dictionary* dict,
                     UserDictionary* user_dict,
                     size_t seg_start,
                     size_t seg_end,
                     map<pair<size_t, size_t>, an<WordGraph>>* cache,
                     WordGraph* graph);
  bool PhrasesExhausted() const;
  // 還沒放英文候選時的第一候選（判斷用；不改變之後出候選的順序）
  an<Phrase> PeekFirstCandidate();

  ScriptTranslator* translator_;
  Poet* poet_;
  string input_;
  size_t start_;
  size_t end_of_input_;
  an<ScriptSyllabifier> syllabifier_;

  an<DictEntryCollector> phrase_;
  an<UserDictEntryCollector> user_phrase_;
  deque<an<Sentence>> sentences_;
  deque<an<Phrase>> english_;
  // 含單字表外字母串的英文句、數字（沒排第一的）：排在第 english_letter_position 個
  deque<an<Phrase>> english_late_;
  // 判斷說英文句比原本的第一候選通順：english_ 的第一句排第一
  bool english_first_ = false;

  an<Phrase> candidate_ = nullptr;
  size_t candidate_index_ = 0;
  size_t chinese_index_ = 0;  // 出過幾個英文以外的候選
  enum CandidateSource {
    kUninitialized,
    kUserPhrase,
    kSysPhrase,
    kSentence,
    kEnglish,
    kEnglishLate,
  };
  CandidateSource candidate_source_ = kUninitialized;

  DictEntryCollector::reverse_iterator phrase_iter_;
  UserDictEntryCollector::reverse_iterator user_phrase_iter_;

  const size_t max_corrections_ = 4;
  size_t correction_count_ = 0;
  int max_sentences_ = 1;
  double sentence_cutoff_threshold_ = 0.1;

  Corrector* corrector_;
  bool enable_correction_;
};

// ScriptTranslator implementation

ScriptTranslator::ScriptTranslator(const Ticket& ticket)
    : Translator(ticket), Memory(ticket), TranslatorOptions(ticket) {
  if (!engine_)
    return;
  if (Config* config = engine_->schema()->config()) {
    config->GetInt(name_space_ + "/spelling_hints", &spelling_hints_);
    config->GetInt(name_space_ + "/max_word_length", &max_word_length_);
    config->GetInt(name_space_ + "/core_word_length", &core_word_length_);
    config->GetBool(name_space_ + "/always_show_comments",
                    &always_show_comments_);
    config->GetBool(name_space_ + "/enable_correction", &enable_correction_);
    if (!config->GetBool(name_space_ + "/enable_word_completion",
                         &enable_word_completion_)) {
      enable_word_completion_ = enable_completion_;
    }
    config->GetInt(name_space_ + "/max_homophones", &max_homophones_);
    config->GetInt(name_space_ + "/max_user_phrase_syllables",
                   &max_user_phrase_syllables_);
    // 蓬萊：英文單字進整句。輸入裡連續的小寫字母有一段是單字表裡的詞時，
    // 多組一句「前面照常組中文＋英文照字面＋後面照常組中文」，排在第 english_position 個候選
    string english_lexicon;
    if (config->GetString(name_space_ + "/english_lexicon", &english_lexicon) &&
        !english_lexicon.empty()) {
      config->GetInt(name_space_ + "/english_min_length", &english_min_length_);
      config->GetDouble(name_space_ + "/english_min_zipf", &english_min_zipf_);
      config->GetInt(name_space_ + "/english_position", &english_position_);
      config->GetInt(name_space_ + "/english_max_sentences",
                     &english_max_sentences_);
      config->GetDouble(name_space_ + "/english_weight", &english_weight_);
      english_lexicon_ = EnglishLexicon::Require(
          english_lexicon, english_min_length_, english_min_zipf_);
      // 中英混輸的判斷：判斷表（tools/gen_judge.py）、τ、第一候選有學過幾字以上的詞就不換
      string english_judge;
      if (english_lexicon_ &&
          config->GetString(name_space_ + "/english_judge", &english_judge) &&
          !english_judge.empty()) {
        english_judge_ = EnglishLexicon::RequireJudge(english_judge);
      }
      config->GetDouble(name_space_ + "/english_judge_tau", &english_judge_tau_);
      config->GetInt(name_space_ + "/english_judge_learned",
                     &english_judge_learned_);
      // 單字表以外的連續字母串（頭尾各少 0–english_letter_trim 個字母）也當英文，只在有判斷表時
      config->GetBool(name_space_ + "/english_letter_runs",
                      &english_letter_runs_);
      config->GetInt(name_space_ + "/english_letter_trim",
                     &english_letter_trim_);
      config->GetDouble(name_space_ + "/english_letter_penalty",
                        &english_letter_penalty_);
      config->GetInt(name_space_ + "/english_letter_position",
                     &english_letter_position_);
      config->GetInt(name_space_ + "/english_judge_readings",
                     &english_judge_readings_);
      // 整段都是數字鍵（大千的 0–9 是注音），中文組不通時退回成數字，只在有判斷表時
      config->GetBool(name_space_ + "/digit_runs", &digit_runs_);
      config->GetInt(name_space_ + "/digit_min_length", &digit_min_length_);
      config->GetDouble(name_space_ + "/digit_penalty", &digit_penalty_);
      // 數字串裡連著的聲調鍵（speller 收進來的，speller/digit_tones）
      config->GetBool("speller/digit_tones", &digit_tones_);
      // 開頭是聲調鍵的數字（speller/digit_lead_tones，abc_segmentor 收成一段）
      config->GetBool("speller/digit_lead_tones", &digit_lead_tones_);
      config->GetString("speller/finals", &speller_finals_);
    }
    poet_.reset(new Poet(language(), config));
    // Jason 原型：學習紀錄排序公式、撤銷學習的時間窗
    auto& sc = UserDictionary::scoring();
    config->GetInt(name_space_ + "/revert_learning_seconds", &sc.revert_window);
    string mode;
    if (config->GetString(name_space_ + "/user_scoring/mode", &mode)) {
      sc.mode = (mode == "frequency") ? 1 : 0;
      config->GetDouble(name_space_ + "/user_scoring/recency", &sc.recency);
      config->GetDouble(name_space_ + "/user_scoring/unit", &sc.unit);
      config->GetDouble(name_space_ + "/user_scoring/prior_weight",
                        &sc.prior_weight);
      string prior_file;
      if (config->GetString(name_space_ + "/user_scoring/prior_file",
                            &prior_file) &&
          sc.prior.empty()) {
        path file = Service::instance().deployer().user_data_dir / prior_file;
        std::ifstream in(file.string());
        string word;
        double count;
        while (in >> word >> count) {
          sc.prior[word] = count;
        }
        LOG(INFO) << "user_scoring prior: " << sc.prior.size() << " entries from "
                  << file;
      }
      LOG(INFO) << "user_scoring mode=" << sc.mode << " recency=" << sc.recency
                << " unit=" << sc.unit << " prior_weight=" << sc.prior_weight;
    }
  }
  // 蓬萊：忘掉上一筆。輸入剛好是 forget_last_code 時第一候選是「〔忘掉：上一筆〕」，
  // 選它（空白鍵、Shift+1）或送出它（Return）就刪掉上一筆送出寫進的學習紀錄，不送出文字
  if (Config* config = engine_->schema()->config()) {
    if (config->GetString(name_space_ + "/forget_last_code",
                          &forget_last_code_) &&
        !forget_last_code_.empty()) {
      config->GetInt(name_space_ + "/forget_last_history",
                     &forget_last_history_);
      Context* ctx = engine_->context();
      // 接在 Memory 之後：送出時先寫學習紀錄，再記下這一筆
      forget_commit_connection_ = ctx->commit_notifier().connect(
          [this](Context* ctx) { OnCommitForForget(ctx); });
      forget_select_connection_ = ctx->select_notifier().connect(
          [this](Context* ctx) { OnSelectForForget(ctx); });
    }
  }
  // 蓬萊：詞典的變體表（Dictionary::SelectVariant）跟著方案選項切換：方案 switches 裡有選項的名稱
  // 剛好是變體名稱（詞典檔的 weight@<名稱> 欄）時，那個選項打開就查那張表；都沒打開就是主表
  if (Config* config = engine_->schema()->config()) {
    if (dict_) {
      Switches switches(config);
      switches.FindOption([this](Switches::SwitchOption option) {
        variant_options_.push_back(option.option_name);
        return Switches::kContinue;
      });
    }
  }
  if (enable_correction_) {
    if (auto* corrector = Corrector::Require("corrector")) {
      corrector_.reset(corrector->Create(ticket));
    }
  }
  // the main translator's preedit_format also applies to raw input left
  // uncovered (after the caret, or after a shorter candidate)
  if (name_space_ == "translator") {
    Config* config = engine_->schema()->config();
    auto projection = New<Projection>();
    if (config &&
        projection->Load(config->GetList(name_space_ + "/preedit_format"))) {
      raw_input_format_ = New<RawInputFormat>();
      raw_input_format_->projection = projection;
      raw_input_format_->delimiter = delimiters_.substr(0, 1);
      engine_->context()->set_raw_input_format(raw_input_format_);
    }
  }
}

ScriptTranslator::~ScriptTranslator() {
  forget_select_connection_.disconnect();
  forget_commit_connection_.disconnect();
  if (engine_ && raw_input_format_ &&
      engine_->context()->raw_input_format() == raw_input_format_) {
    engine_->context()->set_raw_input_format(nullptr);
  }
}

an<Translation> ScriptTranslator::Query(const string& input,
                                        const Segment& segment) {
  // 蓬萊：忘掉上一筆的指令碼可能不是合法的拼法（77 是兩個輕聲鍵，不會標成 abc），所以先檢查
  an<Translation> forget;
  if (!forget_last_code_.empty() && input == forget_last_code_ &&
      segment.start == 0 &&
      segment.end == engine_->context()->input().length()) {
    const auto& history = forget_history();
    string comment = history.empty()
                         ? string("〔沒有可忘掉的紀錄〕")
                         : "〔忘掉：" + history.back().text + "〕";
    // 候選文字是空的：真的送出也只送出空字串
    auto cand = New<SimpleCandidate>("forget_last", segment.start, segment.end,
                                     string(), comment);
    cand->set_quality(1000);
    forget = New<UniqueTranslation>(cand);
  }
  // 蓬萊：開頭是聲調鍵的數字（speller/digit_lead_tones）
  if (segment.start == 0 && DigitLeadReady()) {
    if (segment.HasTag("raw")) {
      // 開頭的聲調鍵單獨成一段：告訴 abc_segmentor 接著打數字鍵時可以收成一段（上游 reverse_lookup 也這樣改 prompt）
      bool tones = !input.empty();
      for (char c : input) {
        tones = tones && c >= '0' && c <= '9' &&
                speller_finals_.find(c) != string::npos;
      }
      if (tones)
        const_cast<Segment&>(segment).tags.insert(kDigitLeadTag);
    } else if (segment.HasTag(kDigitLeadTag)) {
      // abc_segmentor 收成一段的（只標 digit_lead，其他翻譯器不查）
      auto digits = TranslateDigitLead(input, segment);
      return forget ? forget + digits : digits;
    }
  }
  if (!dict_ || !dict_->loaded())
    return forget;
  if (!segment.HasAnyTagIn(tags_))
    return forget;
  DLOG(INFO) << "input = '" << input << "', [" << segment.start << ", "
             << segment.end << ")";

  FinishSession();
  RefreshBlacklist();
  SelectDictionaryVariant();

  bool enable_user_dict =
      user_dict_ && user_dict_->loaded() && !IsUserDictDisabledFor(input);

  size_t end_of_input = engine_->context()->input().length();
  // the translator should survive translations it creates
  auto result = New<ScriptTranslation>(
      this, corrector_.get(), poet_.get(), input, segment.start, end_of_input,
      max_sentences_, sentence_cutoff_threshold_);
  if (!result || !result->Evaluate(
                     dict_.get(), enable_user_dict ? user_dict_.get() : NULL)) {
    return forget;
  }
  an<Translation> deduped = New<DistinctTranslation>(result);
  if (contextual_suggestions_) {
    deduped = poet_->ContextualWeighted(deduped, input, segment.start, this);
  }
  if (forget)
    return forget + deduped;
  return deduped;
}

void ScriptTranslator::SelectDictionaryVariant() {
  if (variant_options_.empty())
    return;
  Context* ctx = engine_->context();
  string variant;
  for (const auto& option : variant_options_) {
    if (ctx->get_option(option) && dict_->HasVariant(option)) {
      variant = option;
      break;
    }
  }
  dict_->SelectVariant(variant);
}

int ScriptTranslator::core_word_length() const {
  if (max_word_length_ <= 0) {
    return core_word_length_;
  }
  return (std::min)(core_word_length_, max_word_length_);
}

static bool exceed_upperlimit(int length, int upper_limit) {
  return upper_limit > 0 && length > upper_limit;
}

bool ScriptTranslator::SaveCommitEntry(CommitEntry& commit_entry) {
  if (exceed_upperlimit(commit_entry.Length(), max_word_length())) {
    UpdateElements(commit_entry);
  } else {
    commit_entry.Save();
  }
  return true;
}

bool ScriptTranslator::ConcatenatePhrases(CommitEntry& commit_entry,
                                          const vector<an<Phrase>>& phrases) {
  const int kCoreWordLength = core_word_length();
  const int n = phrases.size();
  for (int i = 0; i < n; ++i) {
    int cur_len = 0;
    int j = i;
    for (; j < n; ++j) {
      commit_entry.AppendPhrase(phrases.at(j));
      cur_len += phrases.at(j)->code().size();
      if (kCoreWordLength <= 0 || cur_len > kCoreWordLength) {
        break;
      }
      SaveCommitEntry(commit_entry);
    }
    if (kCoreWordLength > 0) {
      if (j == i) {
        SaveCommitEntry(commit_entry);
      }
      commit_entry.Clear();
    }
  }
  SaveCommitEntry(commit_entry);
  commit_entry.Clear();

  return true;
}

bool ScriptTranslator::ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                              const Segment& seg) {
  auto phrase =
      As<Phrase>(Candidate::GetGenuineCandidate(seg.GetSelectedCandidate()));
  bool recognized = Language::intelligible(phrase, this);
  if (recognized) {
    queue_.push_back(phrase);
  }

  if (!recognized || seg.status >= Segment::kConfirmed) {
    ConcatenatePhrases(commit_entry, queue_);
    queue_.clear();
  }

  return true;
}

string ScriptTranslator::FormatPreedit(const string& preedit) {
  string result = preedit;
  preedit_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::Spell(const Code& code) {
  string result;
  vector<string> syllables;
  if (!dict_ || !dict_->Decode(code, &syllables) || syllables.empty())
    return result;
  result = boost::algorithm::join(syllables, string(1, delimiters_.at(0)));
  comment_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::GetPrecedingText(size_t start) const {
  return !contextual_suggestions_ ? string()
         : start > 0 ? engine_->context()->composition().GetTextBefore(start)
                     : engine_->context()->commit_history().latest_text();
}

bool ScriptTranslator::UpdateElements(const CommitEntry& commit_entry) {
  bool update_elements = false;
  // avoid updating single character entries within a phrase which is
  // composed with single characters only
  if (commit_entry.elements.size() > 1) {
    for (const DictEntry* e : commit_entry.elements) {
      if (e->code.size() > 1) {
        update_elements = true;
        break;
      }
    }
  }
  if (update_elements) {
    for (const DictEntry* e : commit_entry.elements) {
      // 蓬萊：沒有碼的英文詞（詞典裡沒有它的假音節）不寫
      if (e->code.empty())
        continue;
      WriteUserEntry(*e, 0);
    }
  }
  return true;
}

bool ScriptTranslator::ExcludeOnDelete(const an<Phrase>& phrase) {
  ExcludedWords* excluded = excluded_words();
  if (!excluded || !phrase || phrase->type() != "phrase")
    return false;
  const DictEntry& entry = phrase->entry();
  if (entry.commit_count > 0)
    return false;
  int length = 0;
  for (unsigned char c : entry.text)
    length += (c & 0xC0) != 0x80;
  if (length < dictionary_exclude_min_length())
    return false;
  if (!excluded->Add(entry.text))
    return false;
  LOG(INFO) << "dictionary_exclude_file: excluded '" << entry.text << "'.";
  return true;
}

bool ScriptTranslator::Memorize(const CommitEntry& commit_entry) {
  UpdateElements(commit_entry);
  // 蓬萊：有沒有碼的英文詞時，整句的碼少了那幾個字母，不寫整句
  for (const DictEntry* e : commit_entry.elements) {
    if (e && e->code.empty() && !e->text.empty())
      return true;
  }
  WriteUserEntry(commit_entry, 1);
  return true;
}

void ScriptTranslator::WriteUserEntry(const DictEntry& entry, int commits) {
  user_dict_->UpdateEntry(entry, commits);
  if (!forget_last_code_.empty())
    pending_.push_back({entry.text, entry.code, commits > 0});
}

deque<ScriptTranslator::Remembered>& ScriptTranslator::forget_history() {
  // 所有 session 共用（在 A 程式送出、到 B 程式忘掉也可以），照學習紀錄分開
  static map<string, deque<Remembered>> histories;
  return histories[user_dict_ ? user_dict_->name() : string()];
}

bool ScriptTranslator::ForgetSelected(Context* ctx) const {
  for (const auto& seg : ctx->composition()) {
    auto cand = Candidate::GetGenuineCandidate(seg.GetSelectedCandidate());
    if (cand && cand->type() == "forget_last")
      return true;
  }
  return false;
}

void ScriptTranslator::OnSelectForForget(Context* ctx) {
  if (!ForgetSelected(ctx))
    return;
  ForgetLast(ctx);
  ctx->Clear();
}

void ScriptTranslator::OnCommitForForget(Context* ctx) {
  if (ForgetSelected(ctx)) {
    pending_.clear();
    ForgetLast(ctx);
    return;
  }
  if (pending_.empty())
    return;
  auto& history = forget_history();
  history.push_back({ctx->GetCommitText(), std::move(pending_)});
  pending_.clear();
  while (!history.empty() &&
         static_cast<int>(history.size()) > (std::max)(1, forget_last_history_))
    history.pop_front();
}

// 刪掉上一筆送出寫進的學習紀錄：整筆一律刪；整句裡的詞只刪送出次數還是 0 的
// （自己單獨選過的詞保留）。結果放在 context 的 forget_last 屬性：
// 「forgotten<Tab>文字」「gone<Tab>文字」（學習紀錄裡已經沒有）「none<Tab>」（沒有可忘掉的）
void ScriptTranslator::ForgetLast(Context* ctx) {
  auto& history = forget_history();
  if (history.empty() || !user_dict_ || user_dict_->readonly()) {
    ctx->set_property("forget_last", "none\t");
    return;
  }
  Remembered item = std::move(history.back());
  history.pop_back();
  FinishSession();  // 送出的交易先寫進去，才不會被之後的寫入蓋回來
  set<string> wholes;
  for (const auto& w : item.written) {
    if (w.whole)
      wholes.insert(w.text);
  }
  int removed = 0;
  set<pair<string, Code>> done;
  for (const auto& w : item.written) {
    if (!done.insert({w.text, w.code}).second)
      continue;
    if (!w.whole && wholes.count(w.text))
      continue;
    DictEntry entry;
    entry.text = w.text;
    entry.code = w.code;
    int commits = 0;
    if (!user_dict_->FetchCommits(entry, &commits))
      continue;
    if (!w.whole && commits > 0)
      continue;
    user_dict_->UpdateEntry(entry, -1);
    ++removed;
  }
  FinishSession();
  LOG(INFO) << "forget_last: '" << item.text << "', " << removed
            << " entries removed.";
  ctx->set_property("forget_last",
                    (removed > 0 ? "forgotten\t" : "gone\t") + item.text);
}

bool ScriptTranslator::HasDigitTonePair(const string& input) const {
  if (!digit_tones_)
    return false;
  for (size_t i = 1; i < input.length(); ++i) {
    if (speller_finals_.find(input[i]) != string::npos &&
        speller_finals_.find(input[i - 1]) != string::npos)
      return true;
  }
  return false;
}

bool ScriptTranslator::EnglishCode(const string& word, Code* code) {
  if (!english_letters_ready_) {
    english_letters_ready_ = true;
    english_letters_.assign(26, -1);
    if (dict_ && dict_->loaded() && dict_->prism()) {
      Prism& prism = *dict_->prism();
      for (int i = 0; i < 26; ++i) {
        int spelling_id = -1;
        if (!prism.GetValue(string(1, 'a' + i), &spelling_id))
          continue;
        for (SpellingAccessor accessor(prism.QuerySpelling(spelling_id));
             !accessor.exhausted(); accessor.Next()) {
          Code one;
          one.push_back(accessor.syllable_id());
          vector<string> syllables;
          if (dict_->Decode(one, &syllables) && syllables.size() == 1 &&
              syllables[0].length() > 1 && syllables[0].back() == '~') {
            english_letters_[i] = accessor.syllable_id();
            break;
          }
        }
      }
    }
  }
  Code result;
  for (char c : word) {
    if (c < 'a' || c > 'z' || english_letters_[c - 'a'] < 0)
      return false;
    result.push_back(english_letters_[c - 'a']);
  }
  *code = std::move(result);
  return true;
}

// ScriptSyllabifier implementation

Spans ScriptSyllabifier::Syllabify(const Phrase* phrase) {
  Spans result;
  vector<size_t> vertices;
  vertices.push_back(start_);
  SyllabifyTask task{
      phrase->code(), syllable_graph_, phrase->end() - start_,
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) { vertices.push_back(start_ + next_pos); },
      [&](SyllabifyTask* task, size_t depth) { vertices.pop_back(); }};
  if (syllabify_dfs(&task, 0, phrase->start() - start_)) {
    result.set_vertices(std::move(vertices));
  }
  return result;
}

size_t ScriptSyllabifier::BuildSyllableGraph(Prism& prism) {
  return (size_t)syllabifier_.BuildSyllableGraph(input_, prism,
                                                 &syllable_graph_);
}

bool ScriptSyllabifier::IsCorrection(const Code& code,
                                     size_t code_length) const {
  vector<bool> path_attributes;
  path_attributes.reserve(8);

  SyllabifyTask task{
      code, syllable_graph_, code_length,
      // push
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) {
        auto id = task->code[depth];

        auto start_iter = syllable_graph_.edges.find(current_pos);
        if (start_iter != syllable_graph_.edges.end()) {
          auto end_iter = start_iter->second.find(next_pos);
          if (end_iter != start_iter->second.end()) {
            auto prop_iter = end_iter->second.find(id);
            if (prop_iter != end_iter->second.end()) {
              path_attributes.push_back(prop_iter->second.is_correction);
              return;
            }
          }
        }
        // edge not found
        path_attributes.push_back(false);
      },
      // pop
      [&](SyllabifyTask* task, size_t depth) { path_attributes.pop_back(); }};

  if (syllabify_dfs(&task, 0, 0)) {
    for (bool is_correction : path_attributes) {
      if (is_correction)
        return true;
    }
  }
  return false;
}

string ScriptSyllabifier::GetPreeditString(const Phrase& cand) const {
  const auto& delimiters = translator_->delimiters();
  std::stack<size_t> lengths;
  string output;
  SyllabifyTask task{cand.matching_code(), syllable_graph_, cand.end() - start_,
                     [&](SyllabifyTask* task, size_t depth, size_t current_pos,
                         size_t next_pos) {
                       size_t len = output.length();
                       if (depth > 0 && len > 0 &&
                           delimiters.find(output[len - 1]) == string::npos) {
                         output += delimiters.at(0);
                       }
                       output +=
                           input_.substr(current_pos, next_pos - current_pos);
                       lengths.push(len);
                     },
                     [&](SyllabifyTask* task, size_t depth) {
                       output.resize(lengths.top());
                       lengths.pop();
                     }};
  if (syllabify_dfs(&task, 0, cand.start() - start_)) {
    return translator_->FormatPreedit(output);
  } else {
    return string();
  }
}

string ScriptSyllabifier::GetOriginalSpelling(const Phrase& cand) const {
  if (translator_ &&
      static_cast<int>(cand.code().size()) <= translator_->spelling_hints()) {
    return translator_->Spell(cand.code());
  }
  return string();
}

template <class Ptr, class Iter>
static bool has_exact_match_phrase(Ptr ptr, Iter iter, size_t consumed) {
  return ptr && iter->first == consumed && !iter->second.exhausted() &&
         iter->second.Peek()->IsExactMatch();
}

// ScriptTranslation implementation

bool ScriptTranslation::Evaluate(Dictionary* dict, UserDictionary* user_dict) {
  size_t consumed = syllabifier_->BuildSyllableGraph(*dict->prism());
  const auto& syllable_graph = syllabifier_->syllable_graph();
  bool predict_word = translator_->enable_word_completion() &&
                      start_ + consumed == end_of_input_;

  phrase_ =
      dict->Lookup(syllable_graph, 0, &translator_->blacklist(), predict_word);
  if (user_dict) {
    const size_t kUnlimitedDepth = 0;
    const size_t kNumSyllablesToPredictWord = 4;
    user_phrase_ =
        user_dict->Lookup(syllable_graph, 0, kUnlimitedDepth,
                          predict_word ? kNumSyllablesToPredictWord : 0);
  }
  if (!phrase_ && !user_phrase_)
    return false;

  if (phrase_)
    phrase_iter_ = phrase_->rbegin();
  if (user_phrase_)
    user_phrase_iter_ = user_phrase_->rbegin();

  auto is_correction_match = [&](auto& iter_pair, size_t len) {
    if (iter_pair.first != len || iter_pair.second.exhausted())
      return false;
    return syllabifier_->IsCorrection(iter_pair.second.Peek()->code, len);
  };

  bool has_reliable_phrase =
      has_exact_match_phrase(phrase_, phrase_iter_, consumed) &&
      !is_correction_match(*phrase_iter_, consumed);

  bool has_reliable_user_phrase =
      has_exact_match_phrase(user_phrase_, user_phrase_iter_, consumed) &&
      !is_correction_match(*user_phrase_iter_, consumed);

  bool has_at_least_two_syllables = syllable_graph.edges.size() >= 2;
  DLOG(INFO) << "consumed: " << consumed
             << ", has_reliable_phrase: " << has_reliable_phrase
             << ", has_reliable_user_phrase: " << has_reliable_user_phrase
             << ", has_at_least_two_syllables: " << has_at_least_two_syllables;
  // make sentences when there is no exact-matching phrase candidate
  if (has_at_least_two_syllables && !has_reliable_phrase &&
      !has_reliable_user_phrase) {
    if (max_sentences_ > 1)
      sentences_ = MakeSentences(dict, user_dict);
    else if (max_sentences_) {
      auto sentence = MakeSentence(dict, user_dict);
      if (sentence)
        sentences_ = {sentence};
      else
        sentences_.clear();
    }
  }
  if (translator_->english_lexicon())
    MakeEnglishCandidates(dict, user_dict);

  return !CheckEmpty();
}

bool ScriptTranslation::Next() {
  if (!exhausted() && candidate_source_ == kUninitialized)
    PrepareCandidate();
  const bool english_popped =
      candidate_source_ == kEnglish || candidate_source_ == kEnglishLate;
  do {
    if (exhausted())
      return false;
    if (candidate_source_ == kUninitialized) {
      PrepareCandidate();  // to determine candidate_source_
    }
    switch (candidate_source_) {
      case kUninitialized:
        break;
      case kSentence:
        if (!sentences_.empty())
          sentences_.pop_front();
        break;
      case kEnglish:
        if (!english_.empty())
          english_.pop_front();
        break;
      case kEnglishLate:
        if (!english_late_.empty())
          english_late_.pop_front();
        break;
      case kUserPhrase: {
        UserDictEntryIterator& uter(user_phrase_iter_->second);
        if (!uter.Next()) {
          ++user_phrase_iter_;
        }
      } break;
      case kSysPhrase: {
        DictEntryIterator& iter(phrase_iter_->second);
        if (!iter.Next()) {
          ++phrase_iter_;
        }
      } break;
    }
    candidate_.reset();
    candidate_source_ = kUninitialized;
    if (enable_correction_) {
      // populate next candidate and skip it if it's a correction beyond max
      // numbers.
      if (!PrepareCandidate()) {
        break;
      }
    }
  } while (enable_correction_ &&
           syllabifier_->IsCorrection(candidate_->code(),
                                      candidate_->end() - start_) &&
           // limit the number of correction candidates
           ++correction_count_ > max_corrections_);
  if (!CheckEmpty()) {
    ++candidate_index_;
    if (!english_popped)
      ++chinese_index_;
    english_first_ = false;
    return true;
  }
  return false;
}

an<Candidate> ScriptTranslation::Peek() {
  if (candidate_source_ == kUninitialized && !PrepareCandidate()) {
    return nullptr;
  }
  if (candidate_->preedit().empty()) {
    candidate_->set_preedit(syllabifier_->GetPreeditString(*candidate_));
  }
  if (candidate_->comment().empty()) {
    auto spelling = syllabifier_->GetOriginalSpelling(*candidate_);
    if (!spelling.empty() && (translator_->always_show_comments() ||
                              spelling != candidate_->preedit())) {
      candidate_->set_comment(/*quote_left + */ spelling /* + quote_right*/);
    }
  }
  if (candidate_source_ != kEnglish && candidate_source_ != kEnglishLate)
    candidate_->set_syllabifier(syllabifier_);
  return candidate_;
}

static bool always_true() {
  return true;
}

template <typename T>
inline static bool prefer_user_phrase(
    T user_phrase_weight,
    T sys_phrase_weight,
    function<bool()> compare_on_tie = always_true) {
  return user_phrase_weight > sys_phrase_weight ||
         (user_phrase_weight == sys_phrase_weight && compare_on_tie());
}

bool ScriptTranslation::PrepareCandidate() {
iter_incremented:
  if (exhausted()) {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
  // 蓬萊：英文句排在第 english_position 個（前面有 english_position − 1 個中文候選）；
  // 判斷說英文句比較通順時第一句排第一；後面沒有別的候選時直接出
  if (!english_.empty() &&
      ((english_first_ && candidate_index_ == 0) ||
       static_cast<int>(chinese_index_) + 1 >=
           translator_->english_position() ||
       (sentences_.empty() && PhrasesExhausted()))) {
    candidate_source_ = kEnglish;
    candidate_ = english_.front();
    return true;
  }
  if (!english_late_.empty() &&
      (static_cast<int>(chinese_index_) + 1 >=
           translator_->english_letter_position() ||
       (sentences_.empty() && PhrasesExhausted()))) {
    candidate_source_ = kEnglishLate;
    candidate_ = english_late_.front();
    return true;
  }
  if (!sentences_.empty()) {
    candidate_source_ = kSentence;
    candidate_ = sentences_[0];
    return true;
  }
  const size_t full_code_length = end_of_input_ - start_;
  size_t user_phrase_code_length = 0;
  if (user_phrase_ && user_phrase_iter_ != user_phrase_->rend()) {
    user_phrase_code_length = user_phrase_iter_->first;
  }
  size_t phrase_code_length = 0;
  if (phrase_ && phrase_iter_ != phrase_->rend()) {
    phrase_code_length = phrase_iter_->first;
  }
  if (user_phrase_code_length > 0 &&
      prefer_user_phrase(
          user_phrase_code_length, phrase_code_length,
          // 編碼長度相同時, 用戶詞優先
          [this, full_code_length, phrase_code_length]() {
            UserDictEntryIterator& uter = user_phrase_iter_->second;
            DictEntryIterator& iter = phrase_iter_->second;
            // 但若用戶詞爲糾錯結果, 則免去優先權, 回歸權重比較
            bool user_is_correction = syllabifier_->IsCorrection(
                uter.Peek()->code, phrase_code_length);
            if (user_is_correction) {
              // 因對糾錯候選施加了較大懲罰, 通常結果爲 系統原文 > 用戶糾錯
              // 兩者皆糾錯時, 用戶詞的動態權重有助達成 用戶糾錯 > 系統糾錯
              return uter.Peek()->weight >= iter.Peek()->weight;
            }
            // 長詞聯想之前須至少出一個嚴格匹配的候選
            // 故確定首選之際, 系統嚴格匹配 > 用戶長詞聯想
            const int kNumExactMatchOnTop = 1;
            return candidate_index_ >= kNumExactMatchOnTop ||
                   prefer_user_phrase(
                       has_exact_match_phrase(user_phrase_, user_phrase_iter_,
                                              full_code_length),
                       has_exact_match_phrase(phrase_, phrase_iter_,
                                              full_code_length));
          })) {
    UserDictEntryIterator& uter = user_phrase_iter_->second;
    const auto& entry = uter.Peek();
    DLOG(INFO) << "user phrase '" << entry->text
               << "', code length: " << user_phrase_code_length;
    candidate_source_ = kUserPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "user_phrase",
                    start_, start_ + user_phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else if (phrase_code_length > 0) {
    DictEntryIterator& iter = phrase_iter_->second;
    if (iter.exhausted()) {
      ++phrase_iter_;
      goto iter_incremented;
    }
    const auto& entry = iter.Peek();
    DLOG(INFO) << "phrase '" << entry->text
               << "', code length: " << phrase_code_length;
    candidate_source_ = kSysPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "phrase",
                    start_, start_ + phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
}

an<Phrase> ScriptTranslation::PeekFirstCandidate() {
  if (candidate_source_ == kUninitialized && !PrepareCandidate())
    return nullptr;
  auto first = candidate_;
  candidate_source_ = kUninitialized;
  candidate_.reset();
  return first;
}

bool ScriptTranslation::PhrasesExhausted() const {
  return (!phrase_ || phrase_iter_ == phrase_->rend()) &&
         (!user_phrase_ || user_phrase_iter_ == user_phrase_->rend());
}

bool ScriptTranslation::CheckEmpty() {
  set_exhausted(PhrasesExhausted() && english_.empty() && english_late_.empty());
  return exhausted();
}

template <class QueryResult>
void ScriptTranslation::EnrollEntries(
    map<int, DictEntryList>& entries_by_end_pos,
    const an<QueryResult>& query_result) {
  if (query_result) {
    for (auto& y : *query_result) {
      DictEntryList& homophones = entries_by_end_pos[y.first];
      while (homophones.size() < translator_->max_homophones() &&
             !y.second.exhausted()) {
        homophones.push_back(y.second.Peek());
        if (!y.second.Next())
          break;
      }
    }
  }
}

WordGraph ScriptTranslation::PrepareForMakingSentence(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const int kMaxSyllablesForUserPhraseQuery =
      translator_->max_user_phrase_syllables();
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph;
  for (const auto& x : syllable_graph.edges) {
    auto& same_start_pos = graph[x.first];
    if (user_dict) {
      EnrollEntries(same_start_pos,
                    user_dict->Lookup(syllable_graph, x.first,
                                      kMaxSyllablesForUserPhraseQuery));
    }
    // merge lookup results
    EnrollEntries(same_start_pos, dict->Lookup(syllable_graph, x.first,
                                               &translator_->blacklist()));
  }
  return graph;
}

deque<an<Sentence>> ScriptTranslation::MakeSentences(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  auto sentences =
      poet_->MakeSentences(graph, syllable_graph.interpreted_length,
                           translator_->GetPrecedingText(start_),
                           max_sentences_, sentence_cutoff_threshold_);
  for (auto& sentence : sentences) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
  }
  return sentences;
}

an<Sentence> ScriptTranslation::MakeSentence(Dictionary* dict,
                                             UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  if (auto sentence =
          poet_->MakeSentence(graph, syllable_graph.interpreted_length,
                              translator_->GetPrecedingText(start_))) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
    return sentence;
  }
  return nullptr;
}

// 蓬萊：英文單字進整句

namespace {

struct EnglishSpan {
  size_t start;
  size_t end;
  double zipf;
  size_t length() const { return end - start; }
  bool in_lexicon = true;  // false：單字表外的字母串（english_letter_runs）或數字
  bool digits = false;     // 整段數字（digit_runs）
};

size_t utf8_length(const string& text) {
  size_t k = 0;
  for (unsigned char c : text)
    k += (c & 0xC0) != 0x80;
  return k;
}

// 判斷用的中文讀法：原本的第一候選或前幾個整句
struct Reading {
  string text;
  string lower;                         // 英文字母轉小寫（比對英文詞用）
  vector<pair<size_t, string>> words;   // （蓋住幾個鍵, 文字），沒蓋住整段輸入時是空的
  double score = 0;
};

// 局部比較：把讀法裡英文段那幾個鍵的字換成英文、其他字不動。全是漢字、鍵數等於字數的詞（首符號）
// 一字一鍵，可以從字中間換；其他詞只能整個換。英文段的邊界對不上時回傳 false
bool ReplaceSpans(const Reading& reading,
                  const vector<EnglishSpan>& config,
                  const string& input,
                  string* out) {
  if (reading.words.empty())
    return false;
  struct Unit {
    size_t start, end;
    string text;
  };
  vector<Unit> units;
  size_t pos = 0;
  for (const auto& w : reading.words) {
    bool han = utf8_length(w.second) == w.first;
    for (unsigned char c : w.second)
      han = han && c >= 0x80;
    if (han) {
      for (size_t i = 0, k = 0; i < w.second.size(); ++k) {
        size_t j = i + 1;
        while (j < w.second.size() && (w.second[j] & 0xC0) == 0x80)
          ++j;
        units.push_back({pos + k, pos + k + 1, w.second.substr(i, j - i)});
        i = j;
      }
    } else {
      units.push_back({pos, pos + w.first, w.second});
    }
    pos += w.first;
  }
  auto on_boundary = [&](size_t p) {
    if (p == pos)
      return true;
    for (const auto& u : units)
      if (u.start == p)
        return true;
    return false;
  };
  for (const auto& s : config) {
    if (s.end > pos || !on_boundary(s.start) || !on_boundary(s.end))
      return false;
  }
  out->clear();
  for (const auto& u : units) {
    bool inside = false;
    for (const auto& s : config) {
      if (s.start == u.start)
        *out += input.substr(s.start, s.length());
      inside = inside || (s.start <= u.start && u.end <= s.end);
    }
    if (!inside)
      *out += u.text;
  }
  return true;
}

// 英文候選的切點是組成它的每個詞的邊界（移游標時一次跳一個詞）
class EnglishSyllabifier : public PhraseSyllabifier {
 public:
  explicit EnglishSyllabifier(vector<size_t> vertices)
      : vertices_(std::move(vertices)) {}
  Spans Syllabify(const Phrase* phrase) override {
    Spans spans;
    spans.set_vertices(vector<size_t>(vertices_));
    return spans;
  }

 private:
  vector<size_t> vertices_;
};

inline bool is_lowercase_letter(char c) {
  return c >= 'a' && c <= 'z';
}

inline bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

}  // namespace

// 跟整段數字鍵（translator/digit_runs）出得了數字候選的條件相同：中英混輸、判斷表、digit_runs 都要有
bool ScriptTranslator::DigitLeadReady() const {
  return digit_lead_tones_ && english_lexicon_ && english_judge_ &&
         digit_runs_ && english_max_sentences_ > 0;
}

// 開頭的聲調鍵打不出中文、後面又都是不帶聲調的數字鍵，只給數字（跟 digit_runs 的整段數字同一種候選，
// 選了不寫學習紀錄）。至少兩位，不看 digit_min_length：那是防兩鍵的中文詞（等等、安裝），開頭是聲調鍵就不會是
an<Translation> ScriptTranslator::TranslateDigitLead(const string& input,
                                                     const Segment& segment) {
  auto entry = New<DictEntry>();
  entry->text = input;  // 數字沒有假音節：碼是空的
  entry->weight = -9.0 * std::log(10.0) + english_weight_;
  entry->preedit = input;
  auto digits = New<Phrase>(language(), "digits", segment.start, segment.end,
                            entry);
  digits->set_syllabifier(New<EnglishSyllabifier>(
      vector<size_t>{segment.start, segment.end}));
  return New<UniqueTranslation>(digits);
}

bool ScriptTranslation::EnrollSegment(
    Dictionary* dict,
    UserDictionary* user_dict,
    size_t seg_start,
    size_t seg_end,
    map<pair<size_t, size_t>, an<WordGraph>>* cache,
    WordGraph* graph) {
  auto key = std::make_pair(seg_start, seg_end);
  auto found = cache->find(key);
  an<WordGraph> part;
  if (found != cache->end()) {
    part = found->second;
  } else {
    // 這一段自己切音節、查詞：整段輸入的切法只要有一條全是正常拼法的路，
    // 就會刪掉簡拼（algo/syllabifier.cc），英文前後的那段可能因此切不出來
    bool is_rear = seg_end == input_.length();
    Syllabifier syllabifier(translator_->delimiters(),
                            is_rear && translator_->enable_completion(),
                            translator_->strict_spelling());
    if (corrector_)
      syllabifier.EnableCorrection(corrector_);
    SyllableGraph syllable_graph;
    string seg = input_.substr(seg_start, seg_end - seg_start);
    size_t consumed =
        syllabifier.BuildSyllableGraph(seg, *dict->prism(), &syllable_graph);
    // 整段都要切得開（英文後面緊接聲調鍵時，後段第一鍵就切不開）
    if (consumed == seg.length()) {
      part = New<WordGraph>();
      for (const auto& x : syllable_graph.edges) {
        map<int, DictEntryList> by_end_pos;
        if (user_dict) {
          EnrollEntries(by_end_pos, user_dict->Lookup(
                                        syllable_graph, x.first,
                                        translator_->max_user_phrase_syllables()));
        }
        EnrollEntries(by_end_pos, dict->Lookup(syllable_graph, x.first,
                                               &translator_->blacklist()));
        auto& same_start_pos = (*part)[seg_start + x.first];
        for (auto& y : by_end_pos) {
          same_start_pos[seg_start + y.first] = std::move(y.second);
        }
      }
    }
    (*cache)[key] = part;
  }
  if (!part)
    return false;
  for (const auto& x : *part) {
    auto& same_start_pos = (*graph)[x.first];
    for (const auto& y : x.second) {
      auto& entries = same_start_pos[y.first];
      entries.insert(entries.end(), y.second.begin(), y.second.end());
    }
  }
  return true;
}

void ScriptTranslation::MakeEnglishCandidates(Dictionary* dict,
                                              UserDictionary* user_dict) {
  const auto& lexicon = translator_->english_lexicon();
  const auto& judge = translator_->english_judge();
  const size_t n = input_.length();
  const size_t min_length =
      static_cast<size_t>((std::max)(1, translator_->english_min_length()));
  // 1. 連續的小寫字母裡，單字表對得上的每一段；有判斷表又開了 english_letter_runs 時，
  //    整串字母（頭尾各少 0–english_letter_trim 個）也算一段，通不通交給判斷
  vector<EnglishSpan> spans;
  for (size_t i = 0; i < n;) {
    if (!is_lowercase_letter(input_[i])) {
      ++i;
      continue;
    }
    size_t run_end = i;
    while (run_end < n && is_lowercase_letter(input_[run_end]))
      ++run_end;
    for (size_t a = i; a < run_end; ++a) {
      for (size_t b = a + min_length;
           b <= run_end && b - a <= lexicon->max_length(); ++b) {
        double zipf = 0.0;
        if (lexicon->Find(input_.substr(a, b - a), &zipf))
          spans.push_back({a, b, zipf});
      }
    }
    if (judge && translator_->english_letter_runs()) {
      const size_t trim = static_cast<size_t>(
          (std::max)(0, translator_->english_letter_trim()));
      for (size_t a = i; a <= i + trim && a < run_end; ++a) {
        for (size_t b = run_end; b + trim >= run_end && b > a; --b) {
          if (b - a < min_length)
            break;
          bool known = false;
          for (const auto& s : spans)
            known = known || (s.start == a && s.end == b);
          // 單字表外的字母串：權重只影響英文句之間的先後，判斷時不看
          if (!known)
            spans.push_back({a, b, translator_->english_min_zipf(), false});
        }
      }
    }
    i = run_end;
  }
  // 1b. 整段都是數字鍵（大千的 0–9 是注音：1ㄅ 2ㄉ 3ˇ 4ˋ 5ㄓ 6ˊ 7˙ 8ㄚ 9ㄞ 0ㄢ）：數字也算一段，通不通交給判斷
  bool digits = judge && translator_->digit_runs() && start_ == 0 &&
                n >= static_cast<size_t>(
                         (std::max)(1, translator_->digit_min_length()));
  for (size_t i = 0; digits && i < n; ++i)
    digits = is_digit(input_[i]);
  if (spans.empty() && !digits)
    return;
  // 收了連著的聲調鍵（speller/digit_tones）：中文切不開整段，只有數字讀得完，直接排第一
  const bool digit_tones = digits && translator_->HasDigitTonePair(input_);

  // 2. 切法：每一段自己一種；最長的 6 段裡互不重疊的兩段一種（一句最多兩段英文，
  //    跟離線模擬 tools/lm/ntc.py 相同）
  vector<vector<EnglishSpan>> configs;
  for (const auto& s : spans)
    configs.push_back({s});
  vector<EnglishSpan> longest(spans);
  std::stable_sort(longest.begin(), longest.end(),
                   [](const EnglishSpan& x, const EnglishSpan& y) {
                     return x.length() > y.length();
                   });
  if (longest.size() > 6)
    longest.resize(6);
  std::sort(longest.begin(), longest.end(),
            [](const EnglishSpan& x, const EnglishSpan& y) {
              return x.start < y.start || (x.start == y.start && x.end < y.end);
            });
  for (size_t p = 0; p < longest.size(); ++p) {
    for (size_t q = p + 1; q < longest.size(); ++q) {
      if (longest[p].end <= longest[q].start)
        configs.push_back({longest[p], longest[q]});
    }
  }
  if (digits) {
    EnglishSpan whole{0, n, 0.0};
    whole.in_lexicon = false;
    whole.digits = true;
    configs.push_back({whole});
  }

  auto make_entry = [this](const EnglishSpan& s) {
    auto entry = New<DictEntry>();
    entry->text = input_.substr(s.start, s.length());
    translator_->EnglishCode(entry->text, &entry->code);
    // 單字的 log 機率（Zipf 是每十億詞的次數取 log10）
    entry->weight =
        (s.zipf - 9.0) * std::log(10.0) + translator_->english_weight();
    return entry;
  };
  const string separator =
      translator_->FormatPreedit(translator_->delimiters().substr(0, 1));

  // 3. 每種切法組一句：中文段各自切音節、查詞，跟英文邊放進同一張詞圖，
  //    由 Poet 跟語言模型一起排，英文前後的中文吃得到彼此當前文
  struct Made {
    an<Phrase> cand;
    const vector<EnglishSpan>* config;
    double score;  // 判斷分數 − τ × 英文段數（沒有判斷表時是整句權重）
    double margin = 0;  // 跟最通順的中文讀法比（局部比較）多出來的分數，> 0 才能排第一
  };
  const string preceding_text = translator_->GetPrecedingText(start_);
  map<pair<size_t, size_t>, an<WordGraph>> cache;
  vector<Made> made;
  an<Phrase> word;
  for (const auto& config : configs) {
    const auto& last = config.back();
    if (config.size() == 1 && last.start == 0) {
      if (last.end == n) {
        // 整段就是英文詞（Poet 不收只有一個詞的整句）：當成一個完整的英文候選
        auto entry = make_entry(last);
        entry->preedit = entry->text;
        auto whole = New<Phrase>(translator_->language(),
                                 last.digits ? "digits" : "english", start_,
                                 start_ + n, entry);
        whole->set_syllabifier(
            New<EnglishSyllabifier>(vector<size_t>{start_, start_ + n}));
        made.push_back({whole, &config, entry->weight});
        continue;
      }
      // 開頭就是英文詞：另給那個詞（不是整句，不參加判斷，排在英文句後面）。
      // 單字表外的字母串、數字不另給（它們排在 english_letter_position，開頭那段不該擠到第二）
      if (last.in_lexicon && (!word || last.length() > word->end() - start_)) {
        auto entry = make_entry(last);
        entry->preedit = entry->text;
        word = New<Phrase>(translator_->language(), "english", start_,
                           start_ + last.end, entry);
        word->set_syllabifier(New<EnglishSyllabifier>(
            vector<size_t>{start_, start_ + last.end}));
      }
    }
    WordGraph graph;
    size_t pos = 0;
    bool ok = true;
    for (const auto& s : config) {
      if (s.start > pos &&
          !EnrollSegment(dict, user_dict, pos, s.start, &cache, &graph)) {
        ok = false;
        break;
      }
      graph[s.start][s.end].push_back(make_entry(s));
      pos = s.end;
    }
    if (ok && pos < n)
      ok = EnrollSegment(dict, user_dict, pos, n, &cache, &graph);
    if (!ok)
      continue;
    auto sentence = poet_->MakeSentence(graph, n, preceding_text);
    if (!sentence || sentence->empty())
      continue;
    // 組字區：中文段照 preedit_format 顯示注音，英文照字面，詞跟詞之間放分隔符號
    string preedit;
    vector<size_t> vertices{start_};
    size_t p = 0;
    for (size_t k = 0; k < sentence->size(); ++k) {
      size_t len = sentence->word_lengths()[k];
      bool english = false;
      for (const auto& s : config)
        english = english || (s.start == p && s.end == p + len);
      if (k > 0)
        preedit += separator;
      preedit += english ? input_.substr(p, len)
                         : translator_->FormatPreedit(input_.substr(p, len));
      p += len;
      vertices.push_back(start_ + p);
    }
    sentence->Offset(start_);
    sentence->set_preedit(preedit);
    sentence->set_syllabifier(New<EnglishSyllabifier>(std::move(vertices)));
    made.push_back({sentence, &config, sentence->weight()});
  }

  // 4. 判斷：英文候選照「判斷分數 − τ × 英文段數 − english_letter_penalty × 單字表外的段數」排
  //    （數字段的分數是每位 log(1/10)，另外扣 digit_penalty）。英文候選裡的英文詞，原本的第一候選都已經有
  //    （不分大小寫）的不要（只是把後面的中文重組得更短）。排第一的條件（10/29 改）：
  //    - 中文那邊取原本的第一候選和前 english_judge_readings 個整句裡判斷分數最高的讀法；已經把同一段讀成
  //      同一個英文詞的讀法不算（它不是另一種中文讀法）。
  //    - 局部比較：把那個讀法裡英文段那幾個鍵的字換成英文、其他字不動，比換之前高才排第一。英文前後的中文
  //      怎麼重組不算進判斷（顯示的還是 Poet 重組的那句）；切點對不上詞的邊界時用重組的那句比。
  //    - 第一候選裡有學過 english_judge_learned 字以上的詞就不換。
  //    - 單字表外的字母串、數字（退回按鍵原本的字）：整段剛好是一個詞（不是組出來的句子）時不當候選，
  //      輪到下一個英文候選判斷。
  bool promote = false;
  bool whole_word = false;
  if (judge) {
    CheckEmpty();  // 建構時是 exhausted，Evaluate 最後才更新；這時 english_ 還是空的
    auto first = PeekFirstCandidate();
    vector<Reading> readings;
    auto add_reading = [&](const an<Phrase>& p) {
      Reading r;
      r.text = p->text();
      for (const auto& x : readings)
        if (x.text == r.text)
          return;
      r.lower = r.text;
      for (char& c : r.lower) {
        if (c >= 'A' && c <= 'Z')
          c = c - 'A' + 'a';
      }
      size_t covered = 0;
      if (auto sentence = As<Sentence>(p)) {
        for (size_t k = 0; k < sentence->size(); ++k) {
          r.words.push_back(
              {sentence->word_lengths()[k], sentence->components()[k].text});
          covered += sentence->word_lengths()[k];
        }
      } else {
        r.words.push_back({p->end() - p->start(), r.text});
        covered = p->end() - p->start();
      }
      if (p->start() != start_ || covered != n)
        r.words.clear();
      r.score = judge->Score(r.text);
      readings.push_back(std::move(r));
    };
    int learned = 0;
    if (first) {
      add_reading(first);
      if (auto sentence = As<Sentence>(first)) {
        for (const auto& e : sentence->components()) {
          if (e.commit_count > 0)
            learned = (std::max)(learned, static_cast<int>(utf8_length(e.text)));
        }
      } else {
        if (first->entry().commit_count > 0)
          learned = static_cast<int>(utf8_length(first->text()));
        whole_word = first->end() == start_ + n;
      }
    }
    const size_t k_readings = static_cast<size_t>(
        (std::max)(0, translator_->english_judge_readings()));
    for (size_t k = 0; k < sentences_.size() && k < k_readings; ++k)
      add_reading(sentences_[k]);
    const double tau = translator_->english_judge_tau();
    vector<Made> kept;
    for (auto& m : made) {
      bool all_in_first = !readings.empty();
      for (const auto& s : *m.config) {
        all_in_first = all_in_first &&
                       readings[0].lower.find(input_.substr(
                           s.start, s.length())) != string::npos;
      }
      if (all_in_first)
        continue;
      const bool is_digits = m.config->front().digits;
      // 數字：每位 log(1/10)（判斷表的分數不看數字）
      const double raw = is_digits ? -std::log(10.0) * static_cast<double>(n)
                                   : judge->Score(m.cand->text());
      double penalty = tau * static_cast<double>(m.config->size());
      for (const auto& s : *m.config) {
        if (s.digits)
          penalty += translator_->digit_penalty();
        else if (!s.in_lexicon)
          penalty += translator_->english_letter_penalty();
      }
      m.score = raw - penalty;
      // 最通順的中文讀法：第一個（原本的第一候選）一定算，其他的已經有這幾個英文詞就不算
      const Reading* best = nullptr;
      for (size_t k = 0; k < readings.size(); ++k) {
        bool same = k > 0;
        for (const auto& s : *m.config) {
          same = same && readings[k].lower.find(input_.substr(
                             s.start, s.length())) != string::npos;
        }
        if (!same && (!best || readings[k].score > best->score))
          best = &readings[k];
      }
      double local = raw;
      string replaced;
      if (best && !is_digits &&
          ReplaceSpans(*best, *m.config, input_, &replaced))
        local = judge->Score(replaced);
      m.margin = best ? local - penalty - best->score
                      : std::numeric_limits<double>::infinity();
      if (is_digits && digit_tones)
        m.margin = std::numeric_limits<double>::infinity();
      kept.push_back(m);
    }
    made.swap(kept);
    // 開頭的英文詞也一樣：原本的第一候選已經有就不另給
    if (word && !readings.empty() &&
        readings[0].lower.find(word->text()) != string::npos)
      word.reset();
    const int guard = translator_->english_judge_learned();
    promote = !(guard > 0 && learned >= guard) || digit_tones;
  }

  // 5. 照分數排（沒有判斷表時是整句權重），跟原本的整句、彼此重複的拿掉，
  //    最多 english_max_sentences 個；開頭的英文詞排在這幾個後面
  // 同分（同一句文字，切法不同）時英文鍵數多的優先：組字區顯示成英文
  auto english_keys = [](const Made& m) {
    size_t k = 0;
    for (const auto& s : *m.config)
      k += s.length();
    return k;
  };
  std::stable_sort(made.begin(), made.end(),
                   [&](const Made& x, const Made& y) {
                     return x.score > y.score ||
                            (x.score == y.score &&
                             english_keys(x) > english_keys(y));
                   });
  set<string> seen;
  for (const auto& sentence : sentences_)
    seen.insert(sentence->text());
  bool decided = false;
  for (const auto& m : made) {
    if (static_cast<int>(english_.size() + english_late_.size()) >=
        translator_->english_max_sentences())
      break;
    // 單字表外的字母串、數字：沒排第一時排在 english_letter_position。第一候選是蓋住整段的一個詞時
    // 不當候選（不佔 english_max_sentences 的名額），輪到下一個英文候選判斷
    bool fallback = false;
    for (const auto& s : *m.config)
      fallback = fallback || !s.in_lexicon;
    if (fallback && whole_word)
      continue;
    if (seen.insert(m.cand->text()).second) {
      // 排第一：排在最前面的那個英文候選，比最通順的中文讀法高
      bool first_here = false;
      if (judge && !decided) {
        decided = true;
        first_here = promote && m.margin > 0;
        english_first_ = first_here;
      }
      if (fallback && !first_here)
        english_late_.push_back(m.cand);
      else
        english_.push_back(m.cand);
    }
  }
  if (word && seen.insert(word->text()).second)
    english_.push_back(word);
}

}  // namespace rime
