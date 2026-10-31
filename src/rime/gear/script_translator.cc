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
#include <optional>
#include <rime/menu.h>
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

// 蓬萊：SentenceCache 每個起點查到的詞（WordGraph 的一列）和查詢讀過的最遠位置（水位）
struct SentenceCacheRow {
  an<map<int, DictEntryList>> entries;
  size_t watermark = 0;
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
  // 蓬萊：按鍵層語言辨識的候選、排在後面的字母串和數字（ScriptTranslator::Query 排位置用，Evaluate 之後才有）
  const vector<an<Phrase>>& keylid_candidates() const { return keylid_; }
  set<const Candidate*> late_candidates() const {
    set<const Candidate*> late;
    for (const auto& c : english_late_)
      late.insert(c.get());
    return late;
  }
  // 蓬萊：拆法比較（同一段輸入的兩種切法比整句分數）。選字時吃幾個鍵用；之後別的切法比較也接這三個
  // 每個起點查詞典、學習紀錄成 WordGraph（音節圖要先建好）。SentenceCache 快照查過、水位沒碰到不同處的列直接沿用
  // （只讀快照）；rows 不是空的時記下每一列
  WordGraph LookupWordGraph(Dictionary* dict,
                            UserDictionary* user_dict,
                            map<int, SentenceCacheRow>* rows);
  // 從音節圖的 from 起跑 Poet 到結尾，第一個詞固定成 first（終點 first_end），回傳整句分數；到不了結尾時沒有值。
  // Poet 的 beam 狀態不能沿用快照：固定第一個詞之後，每個位置的狀態都跟沒固定時不同
  std::optional<double> ScoreSplit(const WordGraph& graph,
                                   size_t from,
                                   const an<DictEntry>& first,
                                   size_t first_end,
                                   const string& preceding_text);
  // 換成 alternative 的條件：有分數，而且比 base 多贏 penalty 以上（base 到不了結尾時有分數就換）
  static bool PreferSplit(std::optional<double> base,
                          std::optional<double> alternative,
                          double penalty) {
    return alternative && (!base || *alternative > *base + penalty);
  }
  // 選字時決定 selected 吃幾個鍵（這份翻譯的輸入從 selected 的起點到整段輸入的結尾）：selected 最後一個詞
  // 在同一個起點有同樣文字、吃更多鍵的詞條時，兩種吃法各算 ScoreSplit，多吃的贏了就回傳換過的候選。
  // evaluated：有沒有走到比較
  an<Phrase> ExtendSelected(Dictionary* dict,
                            UserDictionary* user_dict,
                            const an<Phrase>& selected,
                            const string& preceding_text,
                            bool* evaluated);

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
  // 按鍵層語言辨識（translator/english_keylid）的候選：不從這裡出，ScriptTranslator::Query 另外排
  vector<an<Phrase>> keylid_;

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

// 蓬萊：按鍵層語言辨識的候選（translator/english_keylid）排在第 position 個起、到那一頁結束：前面 position − 1 個
// 照原本的順序；原本在這一頁剩下位置的字母串、數字（english_late_）先往前挪（不被擠到下一頁），接著按鍵層的候選
// （最多 max 個），這一頁其他原本的候選和後面的照原本的順序往後。包在前文重排（ContextualTranslation）外面：
// 重排會把同一段的詞一起按前文排序，插在裡面會把一段切成兩段，前面的順序就變了。
// 數位置時照臺灣字形去重：後面的 simplifier@zh_hant_tw 會把「纔會」換成「才會」，uniquifier 再拿掉重複的，
// 照原本的字數的話，按鍵層的候選會往前補到第 position − 1 個（10/30 tests-i2 有 4 題）
class KeylidTranslation : public Translation {
 public:
  KeylidTranslation(an<Translation> inner,
                    vector<an<Phrase>> keylid,
                    set<const Candidate*> late,
                    an<EnglishJudge> judge,
                    size_t position,
                    size_t page_size,
                    size_t max)
      : inner_(std::move(inner)),
        keylid_(std::move(keylid)),
        late_(std::move(late)),
        judge_(std::move(judge)),
        before_(position > 0 ? position - 1 : 0),
        page_size_((std::max)(static_cast<size_t>(1), page_size)),
        max_(max) {
    set_exhausted(keylid_.empty() && inner_->exhausted());
  }
  bool Next() override {
    if (exhausted())
      return false;
    Fill();
    if (!queue_.empty())
      queue_.pop_front();
    else
      inner_->Next();
    set_exhausted(queue_.empty() && inner_->exhausted());
    return !exhausted();
  }
  an<Candidate> Peek() override {
    if (exhausted())
      return nullptr;
    Fill();
    if (!queue_.empty())
      return queue_.front();
    return inner_->Peek();
  }

 private:
  void Fill() {
    if (filled_)
      return;
    filled_ = true;
    set<string> texts;  // 臺灣字形，數過的
    auto take = [&](size_t* count) {
      auto c = inner_->Peek();
      inner_->Next();
      if (c && texts.insert(judge_ ? judge_->TaiwanVariants(c->text())
                                   : c->text())
                   .second)
        ++*count;
      return c;
    };
    size_t count = 0;
    while (count < before_ && !inner_->exhausted()) {
      if (auto c = take(&count))
        queue_.push_back(c);
    }
    const size_t page_end =
        (before_ / page_size_ + 1) * page_size_;  // 第 position 個那一頁的結尾
    const size_t room = page_end - before_;
    vector<an<Candidate>> late, rest;
    size_t here = 0;
    while (here < room && !inner_->exhausted()) {
      if (auto c = take(&here))
        (late_.count(c.get()) ? late : rest).push_back(c);
    }
    queue_.insert(queue_.end(), late.begin(), late.end());
    const size_t k = (std::min)(
        {keylid_.size(), max_, room > late.size() ? room - late.size() : 0});
    queue_.insert(queue_.end(), keylid_.begin(), keylid_.begin() + k);
    queue_.insert(queue_.end(), rest.begin(), rest.end());
    set_exhausted(queue_.empty() && inner_->exhausted());
  }

  an<Translation> inner_;
  vector<an<Phrase>> keylid_;
  set<const Candidate*> late_;
  an<EnglishJudge> judge_;
  size_t before_;
  size_t page_size_;
  size_t max_;
  bool filled_ = false;
  deque<an<Candidate>> queue_;
};

// 蓬萊：跨按鍵沿用的整句材料。
// 每個鍵 ScriptTranslation 都從頭切音節、對音節圖的每個起點查一次學習紀錄和詞典、Poet 從頭造句，
// 延遲跟已經打的鍵數成正比（10/30 量：第 1 鍵 0.07 ms，第 21–30 鍵 1.3 ms）。這裡留著上一次的音節圖、
// 每個起點查到的詞（WordGraph 的一列）和查詢讀過的最遠位置（水位）、Poet 每個位置的 beam 狀態、
// 英文段自己切音節查到的詞。新的一鍵先找出新舊音節圖第一個不同的位置 D（從那個位置出去的邊不同，
// 或到了任一邊的結尾）：水位 < D 的起點，查詢讀過的邊和結尾的比較都沒變，結果一定相同，直接沿用；
// 其他重查。Poet 沿用第一個重查的起點以前的狀態（Poet::MakeSentences 的 reuse_upto）。
// 詞典、讀音變體、隱藏詞清單、學習紀錄（任何寫入、交易）變了就整個清掉。沿用的詞條是同一批物件：
// WordGraph 的詞條只有 Poet 讀，整句是複製詞條內容，不會改到它們
struct SentenceCache {
  struct Key {
    const Dictionary* dict = nullptr;
    const UserDictionary* user_dict = nullptr;
    string variant;
    int blacklist_version = 0;
    size_t user_version = 0;
    bool operator==(const Key& o) const {
      return dict == o.dict && user_dict == o.user_dict &&
             variant == o.variant && blacklist_version == o.blacklist_version &&
             user_version == o.user_version;
    }
  };
  using Row = SentenceCacheRow;
  // 英文段：起點、終點、那段輸入、是不是最後一段（EnrollSegment）
  using SegmentKey = std::tuple<size_t, size_t, string, bool>;

  // 一次 PrepareForMakingSentence 的結果。全碼打字時，每按一個聲調鍵整段都是正常拼法，Syllabifier
  // 會把整張圖的簡拼刪掉，下一鍵又回來：相鄰兩鍵的圖從頭就不同，所以留最近幾次的，各自比
  struct Snapshot {
    an<ScriptSyllabifier> syllabifier;  // rows 用的音節圖
    map<int, Row> rows;
    an<PoetStates> poet;  // 用這份 rows 算的 beam 狀態（被後來的拿走、或沒算時是空的）
    string poet_context;
    int poet_count = 0;
    size_t poet_total = 0;
  };
  static constexpr size_t kMaxSnapshots = 4;

  Key key;
  deque<Snapshot> snapshots;  // 最近的在前面
  // 這次 PrepareForMakingSentence 算給 MakeSentences 的：拿來接著算的 beam 狀態和能沿用到哪
  an<PoetStates> poet_source;
  int poet_reuse = -1;
  string poet_context;
  int poet_count = 0;
  size_t poet_total = 0;
  map<SegmentKey, an<WordGraph>> segments, last_segments;

  void Reset(const Key& k) {
    key = k;
    snapshots.clear();
    poet_source.reset();
    poet_reuse = -1;
    segments.clear();
    last_segments.clear();
  }

  bool FindSegment(const SegmentKey& k, an<WordGraph>* part) {
    auto found = segments.find(k);
    if (found == segments.end()) {
      found = last_segments.find(k);
      if (found == last_segments.end())
        return false;
      segments[k] = found->second;
    }
    *part = found->second;
    return true;
  }
};

namespace {

bool same_edge(const EdgeProperties& a, const EdgeProperties& b) {
  return a.type == b.type && a.end_pos == b.end_pos &&
         a.credibility == b.credibility && a.tips == b.tips &&
         a.is_correction == b.is_correction &&
         a.ambiguous_source_positions == b.ambiguous_source_positions;
}

bool same_edges(const EndVertexMap& a, const EndVertexMap& b) {
  if (a.size() != b.size())
    return false;
  for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j) {
    if (i->first != j->first || i->second.size() != j->second.size())
      return false;
    for (auto k = i->second.begin(), l = j->second.begin();
         k != i->second.end(); ++k, ++l) {
      if (k->first != l->first || !same_edge(k->second, l->second))
        return false;
    }
  }
  return true;
}

// 新舊音節圖第一個不同的位置：從那個位置出去的邊不同，或到了任一邊的結尾（interpreted_length）。
// 查詞只讀 indices（edges 轉置來的）和 interpreted_length，位置都 < D 時兩邊讀到的一樣
size_t first_difference(const SyllableGraph& a, const SyllableGraph& b) {
  const size_t limit = (std::min)(a.interpreted_length, b.interpreted_length);
  auto i = a.edges.begin();
  auto j = b.edges.begin();
  for (; i != a.edges.end() && j != b.edges.end(); ++i, ++j) {
    if (i->first != j->first)
      return (std::min)({limit, i->first, j->first});
    if (i->first >= limit)
      return limit;
    if (!same_edges(i->second, j->second))
      return i->first;
  }
  if (i != a.edges.end())
    return (std::min)(limit, i->first);
  if (j != b.edges.end())
    return (std::min)(limit, j->first);
  return limit;
}

bool same_entry(const DictEntry& a, const DictEntry& b) {
  return a.text == b.text && a.comment == b.comment && a.preedit == b.preedit &&
         a.code == b.code && a.custom_code == b.custom_code &&
         a.weight == b.weight && a.quality_len == b.quality_len &&
         a.commit_count == b.commit_count &&
         a.remaining_code_length == b.remaining_code_length &&
         a.matching_code_size == b.matching_code_size;
}

bool same_entries(const DictEntryList& a, const DictEntryList& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i] && !same_entry(*a[i], *b[i]))
      return false;
  }
  return true;
}

// 兩份 rows（起點 → 終點 → 詞條）裡，終點 ≤ R 的每一格（有沒有這一格、詞條逐欄位）都相同的最大 R
int same_entries_upto(const map<int, SentenceCache::Row>& a,
                      const map<int, SentenceCache::Row>& b) {
  int first_diff = (std::numeric_limits<int>::max)();
  auto diff_at = [&](int end_pos) {
    if (end_pos < first_diff)
      first_diff = end_pos;
  };
  auto i = a.begin();
  auto j = b.begin();
  while (i != a.end() || j != b.end()) {
    if (j == b.end() || (i != a.end() && i->first < j->first)) {
      if (i->first >= first_diff)
        break;
      if (!i->second.entries->empty())
        diff_at(i->second.entries->begin()->first);
      ++i;
    } else if (i == a.end() || j->first < i->first) {
      if (j->first >= first_diff)
        break;
      if (!j->second.entries->empty())
        diff_at(j->second.entries->begin()->first);
      ++j;
    } else {
      if (i->first >= first_diff)
        break;
      if (i->second.entries != j->second.entries) {
        const auto& x = *i->second.entries;
        const auto& y = *j->second.entries;
        auto k = x.begin();
        auto l = y.begin();
        while ((k != x.end() || l != y.end())) {
          if (l == y.end() || (k != x.end() && k->first < l->first)) {
            diff_at(k->first);
            break;
          }
          if (k == x.end() || l->first < k->first) {
            diff_at(l->first);
            break;
          }
          if (k->first >= first_diff)
            break;
          if (!same_entries(k->second, l->second)) {
            diff_at(k->first);
            break;
          }
          ++k;
          ++l;
        }
      }
      ++i;
      ++j;
    }
  }
  return first_diff == (std::numeric_limits<int>::max)() ? first_diff
                                                         : first_diff - 1;
}

}  // namespace

// ScriptTranslator implementation

ScriptTranslator::ScriptTranslator(const Ticket& ticket)
    : Translator(ticket), Memory(ticket), TranslatorOptions(ticket) {
  sentence_cache_.reset(new SentenceCache);
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
      // 英文後面緊接空白：空白在注音裡是一聲，原本英文那段後面只剩一個一聲、組不成句；打開時空白跟著英文
      config->GetBool(name_space_ + "/english_space", &english_space_);
      // 後面緊接空白的單字表詞最短幾個字母（AI、PR）；沒設（0）跟 english_min_length 一樣。單字表照兩者較短的載入，
      // 其他地方查單字表時另外擋 english_min_length
      config->GetInt(name_space_ + "/english_space_min_length",
                     &english_space_min_length_);
      english_lexicon_ = EnglishLexicon::Require(
          english_lexicon, english_lexicon_min_length(), english_min_zipf_);
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
      // 按鍵層語言辨識（tools/gen_keylid.py 產生的表）：單字表以外的英文段另外組成候選，
      // 排在第 english_keylid_position 個起，不排第一；只在有判斷表時
      string english_keylid;
      if (english_judge_ &&
          config->GetString(name_space_ + "/english_keylid", &english_keylid) &&
          !english_keylid.empty()) {
        english_keylid_ = EnglishLexicon::RequireKeylid(english_keylid);
      }
      config->GetDouble(name_space_ + "/english_keylid_switch",
                        &english_keylid_switch_);
      config->GetInt(name_space_ + "/english_keylid_min_length",
                     &english_keylid_min_length_);
      config->GetDouble(name_space_ + "/english_keylid_margin",
                        &english_keylid_margin_);
      config->GetInt(name_space_ + "/english_keylid_beam", &english_keylid_beam_);
      config->GetInt(name_space_ + "/english_keylid_candidates",
                     &english_keylid_candidates_);
      config->GetInt(name_space_ + "/english_keylid_position",
                     &english_keylid_position_);
      config->GetInt(name_space_ + "/english_keylid_max", &english_keylid_max_);
      keylid_params_.switch_penalty = english_keylid_switch_;
      keylid_params_.min_length = english_keylid_min_length_;
      keylid_params_.margin = english_keylid_margin_;
      keylid_params_.beam = english_keylid_beam_;
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
  // 蓬萊：開頭是這幾個鍵、後面不是聲調鍵時，整段照按鍵原字排第一（大千的 / 是 ㄥ，ㄥ 開頭的音節只有「鞥」，
  // 組不成中文；要的是斜線指令 /loop）。空字串＝關
  if (Config* config = engine_->schema()->config()) {
    if (config->GetString(name_space_ + "/raw_lead_keys", &raw_lead_keys_) &&
        !raw_lead_keys_.empty())
      config->GetString("speller/finals", &speller_finals_);
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
  // 蓬萊：選字時引擎決定選到的詞吃幾個鍵。要比引擎的 OnSelect（照選到的候選切段落）早跑，接在最前面
  if (Config* config = engine_->schema()->config()) {
    config->GetBool(name_space_ + "/extend_selection", &extend_selection_);
    config->GetDouble(name_space_ + "/split_penalty", &split_penalty_);
    if (extend_selection_ && dict_) {
      extend_select_connection_ = engine_->context()->select_notifier().connect(
          [this](Context* ctx) { OnSelectExtend(ctx); },
          boost::signals2::at_front);
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
  extend_select_connection_.disconnect();
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
  // 蓬萊：開頭是 raw_lead_keys 的鍵（/）：組不成中文，只給按鍵原字
  if (segment.start == 0 && RawLead(input)) {
    auto raw = TranslateRawLead(input, segment);
    return forget ? forget + raw : raw;
  }

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
  // 蓬萊：按鍵層語言辨識的候選排在第 english_keylid_position 個起（前文重排之後才插，前面的順序不變）
  if (!result->keylid_candidates().empty()) {
    deduped = New<KeylidTranslation>(
        deduped, result->keylid_candidates(), result->late_candidates(),
        english_judge_,
        static_cast<size_t>((std::max)(1, english_keylid_position_)),
        static_cast<size_t>(engine_->schema()->page_size()),
        static_cast<size_t>((std::max)(0, english_keylid_max_)));
  }
  if (forget)
    return forget + deduped;
  return deduped;
}

SentenceCache* ScriptTranslator::sentence_cache(Dictionary* dict,
                                               UserDictionary* user_dict) {
  SentenceCache::Key key;
  key.dict = dict;
  key.user_dict = user_dict;
  key.variant = dict ? dict->variant() : string();
  key.blacklist_version = blacklist_version();
  key.user_version = user_dict ? user_dict->data_version() : 0;
  if (!(key == sentence_cache_->key))
    sentence_cache_->Reset(key);
  return sentence_cache_.get();
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
  if (!contextual_suggestions_)
    return string();
  if (start > 0)
    return engine_->context()->composition().GetTextBefore(start);
  const auto& history = engine_->context()->commit_history();
  if (!last_commit_as_context_)
    return history.latest_text();
  // 蓬萊：fluency_editor 用 Return 送出時，組字區尾巴的空段落會被記成文字是空字串的 raw，
  // 原版拿它當前文，每一筆新輸入開頭都沒有前文。跳過空的紀錄，接上一筆真的送出的文字
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    if (!it->text.empty())
      return it->text;
  }
  return string();
}

bool ScriptTranslator::UpdateElements(const CommitEntry& commit_entry) {
  // 蓬萊：帶著空白的英文（english_space）整句都不學，句子裡其他有碼的詞也不寫
  // （tailscale 空白組成 Tail＋scale 時，Tail 不該寫進學習紀錄）。Memorize、
  // 超過 max_word_length 的 SaveCommitEntry 都經過這裡；整句本身 Memorize 看到沒有碼的詞就不寫
  if (english_space_) {
    for (const DictEntry* e : commit_entry.elements) {
      if (e && e->code.empty() && !e->text.empty() && e->text.back() == ' ')
        return true;
    }
  }
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

void ScriptTranslator::OnSelectExtend(Context* ctx) {
  Composition& comp = ctx->composition();
  if (comp.empty() || !dict_ || !dict_->loaded())
    return;
  Segment& seg = comp.back();
  auto cand = seg.GetSelectedCandidate();
  auto phrase = As<Phrase>(Candidate::GetGenuineCandidate(cand));
  if (!phrase || !Language::intelligible(phrase, this))
    return;
  const string& type = phrase->type();
  if (type != "phrase" && type != "user_phrase" && type != "sentence")
    return;
  const string& input = ctx->input();
  if (phrase->start() != seg.start || phrase->end() >= input.length())
    return;
  const string rest = input.substr(phrase->start());
  bool enable_user_dict =
      user_dict_ && user_dict_->loaded() && !IsUserDictDisabledFor(rest);
  auto translation = New<ScriptTranslation>(
      this, corrector_.get(), poet_.get(), rest, phrase->start(),
      input.length(), max_sentences_, sentence_cutoff_threshold_);
  bool evaluated = false;
  auto extended = translation->ExtendSelected(
      dict_.get(), enable_user_dict ? user_dict_.get() : nullptr, phrase,
      GetPrecedingText(seg.start), &evaluated);
  if (!evaluated)
    return;
  ++extend_evaluated_;
  seg.tags.insert(kCutHintTag);
  if (extended) {
    ++extend_taken_;
    // 濾鏡換過字形（臺灣字形）的照樣顯示、送出換過的字
    an<Candidate> replacement = extended;
    if (cand != phrase)
      replacement = New<ShadowCandidate>(extended, cand->type(), cand->text(),
                                         cand->comment());
    auto menu = New<Menu>();
    menu->AddTranslation(New<UniqueTranslation>(replacement));
    seg.menu = menu;
    seg.selected_index = 0;
    // 吃到段落後面（游標後）的鍵：段落跟著變長。組字區的輸入原本只到游標，先換成整段（原本的是它的前綴，
    // 不會丟掉任何段落），不然引擎接著重新組字時會把超出去的這段丟掉。原本的長度（length）不動，退格打開時照舊重新組字
    if (replacement->end() > seg.end) {
      if (comp.input().length() < replacement->end())
        comp.Reset(input);
      seg.end = replacement->end();
    }
  }
  ctx->set_property("penglai_extend", std::to_string(extend_evaluated_) + " " +
                                          std::to_string(extend_taken_));
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

WordGraph ScriptTranslation::LookupWordGraph(
    Dictionary* dict,
    UserDictionary* user_dict,
    map<int, SentenceCacheRow>* rows) {
  const int kMaxSyllablesForUserPhraseQuery =
      translator_->max_user_phrase_syllables();
  const auto& syllable_graph = syllabifier_->syllable_graph();
  // 蓬萊：水位比新舊音節圖第一個不同的位置小的起點沿用上一鍵查到的（SentenceCache）
  SentenceCache* cache = translator_->sentence_cache(dict, user_dict);
  vector<size_t> diverge;
  for (const auto& snapshot : cache->snapshots) {
    diverge.push_back(first_difference(
        snapshot.syllabifier->syllable_graph(), syllable_graph));
  }
  WordGraph graph;
  for (const auto& x : syllable_graph.edges) {
    auto& same_start_pos = graph[x.first];
    const SentenceCache::Row* reusable = nullptr;
    for (size_t i = 0; i < cache->snapshots.size() && !reusable; ++i) {
      const auto& old_rows = cache->snapshots[i].rows;
      auto found = old_rows.find(x.first);
      if (found != old_rows.end() && found->second.watermark < diverge[i])
        reusable = &found->second;
    }
    if (reusable) {
      same_start_pos = *reusable->entries;
      if (rows)
        (*rows)[x.first] = *reusable;
      continue;
    }
    size_t watermark = x.first;
    if (user_dict) {
      EnrollEntries(same_start_pos,
                    user_dict->Lookup(syllable_graph, x.first,
                                      kMaxSyllablesForUserPhraseQuery, 0, 0.0,
                                      &watermark));
    }
    // merge lookup results
    EnrollEntries(same_start_pos,
                  dict->Lookup(syllable_graph, x.first,
                               &translator_->blacklist(), false, 0.0,
                               &watermark));
    if (rows)
      (*rows)[x.first] = {New<map<int, DictEntryList>>(same_start_pos),
                          watermark};
  }
  return graph;
}

WordGraph ScriptTranslation::PrepareForMakingSentence(
    Dictionary* dict,
    UserDictionary* user_dict) {
  map<int, SentenceCache::Row> rows;
  WordGraph graph = LookupWordGraph(dict, user_dict, &rows);
  SentenceCache* cache = translator_->sentence_cache(dict, user_dict);
  // Poet：在還留著 beam 狀態的快照裡，挑詞條相同的範圍最長的那份。位置 q 的 beam 狀態只看終點 ≤ q 的詞條
  // （起點都 < q），所以終點 ≤ R 的每一格詞條逐欄位都相同時，狀態沿用到 R（R 也不能是上次或這次的結尾，
  // 在 MakeSentences 裡再限）。這幾格換成那份快照的同一批物件：沿用的狀態指著它們，物件要跟著這次的快照活著
  cache->poet_source.reset();
  cache->poet_reuse = -1;
  int best = 0;  // 沿用不到任何位置的不拿（拿走了那份快照就沒有 beam 狀態了）
  SentenceCache::Snapshot* source = nullptr;
  for (auto& snapshot : cache->snapshots) {
    if (!snapshot.poet)
      continue;
    int same_upto = same_entries_upto(snapshot.rows, rows);
    if (same_upto > best) {
      best = same_upto;
      source = &snapshot;
    }
  }
  if (source) {
    cache->poet_source = source->poet;
    cache->poet_reuse = best;
    cache->poet_context = source->poet_context;
    cache->poet_count = source->poet_count;
    cache->poet_total = source->poet_total;
    for (auto& r : rows) {
      if (r.first >= best)
        break;
      auto found = source->rows.find(r.first);
      if (found == source->rows.end() ||
          found->second.entries == r.second.entries)
        continue;
      an<map<int, DictEntryList>> shared;
      for (const auto& y : *found->second.entries) {
        if (y.first > best)
          break;
        const auto& mine = r.second.entries->at(y.first);
        if (mine.size() == y.second.size() &&
            std::equal(mine.begin(), mine.end(), y.second.begin()))
          continue;
        if (!shared)
          shared = New<map<int, DictEntryList>>(*r.second.entries);
        (*shared)[y.first] = y.second;
      }
      if (shared) {
        r.second.entries = shared;
        graph[r.first] = *shared;
      }
    }
    // 拿走：接著算會改到它，原本那份快照不能再拿來沿用 beam 狀態
    for (auto& snapshot : cache->snapshots) {
      if (snapshot.poet == cache->poet_source)
        snapshot.poet.reset();
    }
  }
  SentenceCache::Snapshot snapshot;
  snapshot.syllabifier = syllabifier_;
  snapshot.rows = std::move(rows);
  cache->snapshots.push_front(std::move(snapshot));
  if (cache->snapshots.size() > SentenceCache::kMaxSnapshots)
    cache->snapshots.pop_back();
  return graph;
}

deque<an<Sentence>> ScriptTranslation::MakeSentences(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  // 蓬萊：Poet 沿用上一鍵的 beam 狀態到第一個換過的起點，也不能是上一次或這一次的結尾
  SentenceCache* cache = translator_->sentence_cache(dict, user_dict);
  const string preceding_text = translator_->GetPrecedingText(start_);
  const size_t total_length = syllable_graph.interpreted_length;
  an<PoetStates> states = cache->poet_source;
  int reuse = -1;
  if (states && preceding_text == cache->poet_context &&
      max_sentences_ == cache->poet_count) {
    reuse = (std::min)({cache->poet_reuse,
                        static_cast<int>(cache->poet_total) - 1,
                        static_cast<int>(total_length) - 1});
  }
  if (!states)
    states = NewPoetStates();
  cache->poet_source.reset();
  auto sentences = poet_->MakeSentences(
      graph, total_length, preceding_text, max_sentences_,
      sentence_cutoff_threshold_, states.get(), reuse);
  // 這次的快照（PrepareForMakingSentence 剛放在最前面）記下 beam 狀態，給後面的鍵沿用
  if (!cache->snapshots.empty()) {
    auto& snapshot = cache->snapshots.front();
    snapshot.poet = states;
    snapshot.poet_context = preceding_text;
    snapshot.poet_count = max_sentences_;
    snapshot.poet_total = total_length;
  }
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

std::optional<double> ScriptTranslation::ScoreSplit(
    const WordGraph& graph,
    size_t from,
    const an<DictEntry>& first,
    size_t first_end,
    const string& preceding_text) {
  const size_t total = syllabifier_->syllable_graph().interpreted_length;
  if (!first || first_end <= from || first_end > total)
    return std::nullopt;
  // 第一個詞就到結尾：Poet 不出只有一個詞的整句，照同一套分數直接算
  if (first_end == total)
    return poet_->Evaluate(preceding_text, *first, true);
  WordGraph forced;
  forced[0][first_end - from].push_back(first);
  for (auto it = graph.lower_bound(first_end); it != graph.end(); ++it) {
    auto& row = forced[it->first - from];
    for (const auto& ev : it->second)
      row[ev.first - from] = ev.second;
  }
  // 跟這段輸入出整句時同一個做法（整句候選數一樣，beam 寬度才一樣）
  if (max_sentences_ > 1) {
    auto sentences =
        poet_->MakeSentences(forced, total - from, preceding_text,
                             max_sentences_, sentence_cutoff_threshold_);
    if (sentences.empty())
      return std::nullopt;
    return sentences.front()->weight();
  }
  if (auto sentence = poet_->MakeSentence(forced, total - from, preceding_text))
    return sentence->weight();
  return std::nullopt;
}

an<Phrase> ScriptTranslation::ExtendSelected(Dictionary* dict,
                                             UserDictionary* user_dict,
                                             const an<Phrase>& selected,
                                             const string& preceding_text,
                                             bool* evaluated) {
  *evaluated = false;
  if (!dict || !dict->prism() || selected->start() != start_)
    return nullptr;
  // 最後一個詞：整句是最後一個成分（前面的詞兩種吃法都一樣，當成前文）；詞就是它自己
  auto sentence = As<Sentence>(selected);
  const DictEntry* last = &selected->entry();
  size_t last_start = selected->start();
  string context = preceding_text;
  if (sentence) {
    const auto& components = sentence->components();
    if (components.empty())
      return nullptr;
    last = &components.back();
    last_start = selected->end() - sentence->word_lengths().back();
    for (size_t i = 0; i + 1 < components.size(); ++i)
      context += components[i].text;
  }
  if (last->code.empty() || last->text.empty())
    return nullptr;
  syllabifier_->BuildSyllableGraph(*dict->prism());
  const auto& graph = syllabifier_->syllable_graph();
  const size_t from = last_start - start_;
  const size_t end = selected->end() - start_;
  if (graph.edges.find(from) == graph.edges.end())
    return nullptr;
  // 同一個起點、同樣文字的詞條，每個終點留分數最高的（學習紀錄的也算，記著是哪邊的）
  map<size_t, pair<an<DictEntry>, bool>> same;
  auto keep = [&](size_t end_pos, const an<DictEntry>& e, bool user) {
    if (!e || e->text != last->text || e->IsPredictiveMatch())
      return;
    auto& slot = same[end_pos];
    if (!slot.first || e->weight > slot.first->weight)
      slot = {e, user};
  };
  if (user_dict) {
    if (auto found = user_dict->Lookup(graph, from)) {
      for (auto& y : *found) {
        for (auto& it = y.second; !it.exhausted();) {
          keep(y.first, it.Peek(), true);
          if (!it.Next())
            break;
        }
      }
    }
  }
  if (auto found = dict->Lookup(graph, from, &translator_->blacklist())) {
    for (auto& y : *found) {
      for (auto& it = y.second; !it.exhausted();) {
        keep(y.first, it.Peek(), false);
        if (!it.Next())
          break;
      }
    }
  }
  if (same.upper_bound(end) == same.end())
    return nullptr;
  *evaluated = true;
  WordGraph words = LookupWordGraph(dict, user_dict, nullptr);
  // 現在的吃法：同一份圖查到的詞條（分數跟多吃的同一套算法）；查不到時用選到的那個
  auto base = same.find(end);
  an<DictEntry> base_entry =
      base != same.end() ? base->second.first : New<DictEntry>(*last);
  auto base_score = ScoreSplit(words, from, base_entry, end, context);
  std::optional<double> best_score;
  size_t best_end = 0;
  pair<an<DictEntry>, bool> best;
  for (auto it = same.upper_bound(end); it != same.end(); ++it) {
    auto score = ScoreSplit(words, from, it->second.first, it->first, context);
    if (score && (!best_score || *score > *best_score)) {
      best_score = score;
      best_end = it->first;
      best = it->second;
    }
  }
  if (!PreferSplit(base_score, best_score, translator_->split_penalty()))
    return nullptr;
  an<Phrase> result;
  if (sentence) {
    auto extended = New<Sentence>(translator_->language());
    const auto& components = sentence->components();
    const auto& lengths = sentence->word_lengths();
    size_t pos = 0;
    for (size_t i = 0; i + 1 < components.size(); ++i) {
      pos += lengths[i];
      extended->Extend(components[i], pos, components[i].weight);
    }
    extended->Extend(*best.first, best_end, *best_score);
    extended->Offset(start_);
    result = extended;
  } else {
    result = New<Phrase>(translator_->language(),
                         best.second ? "user_phrase" : "phrase", start_,
                         start_ + best_end, best.first);
  }
  result->set_syllabifier(syllabifier_);
  result->set_preedit(syllabifier_->GetPreeditString(*result));
  return result;
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
  bool short_word = false; // 比 english_min_length 短、靠後面的空白才收的單字表詞（english_space_min_length）
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

bool ScriptTranslator::RawLead(const string& input) const {
  return !raw_lead_keys_.empty() && !input.empty() &&
         raw_lead_keys_.find(input[0]) != string::npos &&
         (input.length() == 1 ||
          speller_finals_.find(input[1]) == string::npos);
}

// 跟整段數字同一種做法：碼是空的，選了不寫學習紀錄；組字區顯示按鍵原字
an<Translation> ScriptTranslator::TranslateRawLead(const string& input,
                                                   const Segment& segment) {
  auto entry = New<DictEntry>();
  entry->text = input;
  entry->preedit = input;
  auto raw = New<Phrase>(language(), "raw_lead", segment.start, segment.end,
                         entry);
  raw->set_syllabifier(New<EnglishSyllabifier>(
      vector<size_t>{segment.start, segment.end}));
  return New<UniqueTranslation>(raw);
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
    // 蓬萊：同一段輸入（起點、終點、內容、是不是最後一段）切出來、查到的一樣，沿用上一鍵的（SentenceCache）
    SentenceCache* sc = translator_->sentence_cache(dict, user_dict);
    SentenceCache::SegmentKey skey{
        seg_start, seg_end, input_.substr(seg_start, seg_end - seg_start),
        is_rear};
    if (sc->FindSegment(skey, &part)) {
      (*cache)[key] = part;
    } else {
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
      sc->segments[skey] = part;
    }
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
  // 蓬萊：英文段沿用上一鍵切好、查好的（EnrollSegment）；這一鍵沒用到的，下一鍵就丟掉
  {
    SentenceCache* sc = translator_->sentence_cache(dict, user_dict);
    sc->last_segments = std::move(sc->segments);
    sc->segments.clear();
  }
  const auto& lexicon = translator_->english_lexicon();
  const auto& judge = translator_->english_judge();
  const size_t n = input_.length();
  const size_t min_length =
      static_cast<size_t>((std::max)(1, translator_->english_min_length()));
  const size_t space_min_length = static_cast<size_t>(
      (std::max)(1, translator_->english_space_min_length()));
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
    // 後面緊接空白時，整串字母剛好是一個單字表詞的可以短到 english_space_min_length（AI 空白）；
    // 長串裡的一小段不算（auto 空白的 to、xbox 空白的 box 會把對的長詞拆開）
    const bool space_after = translator_->english_space() && run_end < n &&
                             input_[run_end] == ' ';
    const size_t first_min =
        space_after ? (std::min)(min_length, space_min_length) : min_length;
    for (size_t a = i; a < run_end; ++a) {
      for (size_t b = a + first_min;
           b <= run_end && b - a <= lexicon->max_length(); ++b) {
        if (b - a < min_length && (a != i || b != run_end))
          continue;
        double zipf = 0.0;
        if (lexicon->Find(input_.substr(a, b - a), &zipf)) {
          spans.push_back({a, b, zipf});
          spans.back().short_word = b - a < min_length;
        }
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
    // 字母串後面緊接空白：結尾在這裡的英文段把空白也算進去（只剩一個空白的那段本來就組不成）
    if (translator_->english_space() && run_end < n && input_[run_end] == ' ') {
      for (auto& s : spans) {
        if (s.end == run_end)
          s.end = run_end + 1;
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
  // 1c. 按鍵層語言辨識（translator/english_keylid）：有一串連續字母至少 english_keylid_min_length 個才算
  const auto& keylid = translator_->english_keylid();
  bool use_keylid = false;
  if (judge && keylid) {
    const size_t lmin = static_cast<size_t>(
        (std::max)(1, translator_->keylid_params().min_length));
    for (size_t i = 0, run = 0; i < n && !use_keylid; ++i) {
      run = is_lowercase_letter(input_[i]) ? run + 1 : 0;
      use_keylid = run >= lmin;
    }
  }
  if (spans.empty() && !digits && !use_keylid)
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
  // 一種切法組一句（中文段組不出來時回傳 nullptr）
  auto compose = [&](const vector<EnglishSpan>& config) -> an<Sentence> {
    WordGraph graph;
    size_t pos = 0;
    for (const auto& s : config) {
      if (s.start > pos &&
          !EnrollSegment(dict, user_dict, pos, s.start, &cache, &graph))
        return nullptr;
      graph[s.start][s.end].push_back(make_entry(s));
      pos = s.end;
    }
    if (pos < n && !EnrollSegment(dict, user_dict, pos, n, &cache, &graph))
      return nullptr;
    auto sentence = poet_->MakeSentence(graph, n, preceding_text);
    if (!sentence || sentence->empty())
      return nullptr;
    // 組字區：中文段照 preedit_format 顯示注音，英文照字面，詞跟詞之間放分隔符號
    string preedit;
    vector<size_t> vertices{start_};
    size_t p = 0;
    for (size_t k = 0; k < sentence->size(); ++k) {
      size_t len = sentence->word_lengths()[k];
      bool english = false;
      for (const auto& s : config)
        english = english || (s.start == p && s.end == p + len);
      // 帶著空白的英文（english_space）：那個空白就是分隔
      if (k > 0 && !(translator_->english_space() && preedit.back() == ' '))
        preedit += separator;
      preedit += english ? input_.substr(p, len)
                         : translator_->FormatPreedit(input_.substr(p, len));
      p += len;
      vertices.push_back(start_ + p);
    }
    sentence->Offset(start_);
    sentence->set_preedit(preedit);
    sentence->set_syllabifier(New<EnglishSyllabifier>(std::move(vertices)));
    return sentence;
  };
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
      if (last.in_lexicon && !last.short_word &&
          (!word || last.length() > word->end() - start_)) {
        auto entry = make_entry(last);
        entry->preedit = entry->text;
        word = New<Phrase>(translator_->language(), "english", start_,
                           start_ + last.end, entry);
        word->set_syllabifier(New<EnglishSyllabifier>(
            vector<size_t>{start_, start_ + last.end}));
      }
    }
    auto sentence = compose(config);
    if (!sentence)
      continue;
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
  vector<Reading> readings;
  if (judge) {
    CheckEmpty();  // 建構時是 exhausted，Evaluate 最後才更新；這時 english_ 還是空的
    auto first = PeekFirstCandidate();
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
    bool late = false;  // 沒排第一時排在 english_letter_position（短的單字表詞也是：不擠掉第二名的中文）
    for (const auto& s : *m.config) {
      fallback = fallback || !s.in_lexicon;
      late = late || !s.in_lexicon || s.short_word;
    }
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
      if (late && !first_here)
        english_late_.push_back(m.cand);
      else
        english_.push_back(m.cand);
    }
  }
  if (word && seen.insert(word->text()).second)
    english_.push_back(word);

  // 6. 按鍵層語言辨識：同一串按鍵比中文注音鍵模型和英文字母模型（兩狀態分段 Viterbi），強制某一段是英文的最佳切法
  //    跟最佳切法差 english_keylid_margin 以內的，照差距取前 english_keylid_candidates 段（單字表、字母串已經有的
  //    切法不算，輪到下一段）。每段照「強制這段是英文」的那條切法組一句：切法裡其他的英文段也照字面（單字表的詞用
  //    單字表的權重）。原本的第一候選已經有這幾個英文的、跟前面的候選同一句的不要。不判斷、不排第一：
  //    另外放在 keylid_，ScriptTranslator::Query 排在第 english_keylid_position 個起
  if (!use_keylid)
    return;
  auto lattice = keylid->Analyze(input_, translator_->keylid_params());
  set<pair<size_t, size_t>> known;
  for (const auto& s : spans)
    known.insert({s.start, s.end});
  set<vector<pair<size_t, size_t>>> tried;
  const size_t max_keylid = static_cast<size_t>(
      (std::max)(0, translator_->english_keylid_max()));
  int left = translator_->english_keylid_candidates();
  for (const auto& span : lattice->spans()) {
    if (left <= 0 || keylid_.size() >= max_keylid)
      break;
    if (known.count({span.start, span.end}))
      continue;
    --left;
    auto path = lattice->Path(span);
    if (!tried.insert(path).second)
      continue;
    vector<EnglishSpan> config;
    for (const auto& seg : path) {
      double zipf = 0;
      if (seg.second - seg.first >= min_length &&
          lexicon->Find(input_.substr(seg.first, seg.second - seg.first),
                        &zipf))
        config.push_back({seg.first, seg.second, zipf});
      else
        config.push_back(
            {seg.first, seg.second, translator_->english_min_zipf(), false});
    }
    bool all_in_first = !readings.empty();
    for (const auto& s : config) {
      all_in_first =
          all_in_first && readings[0].lower.find(input_.substr(
                              s.start, s.length())) != string::npos;
    }
    if (all_in_first)
      continue;
    an<Phrase> cand;
    if (config.size() == 1 && config[0].start == 0 && config[0].end == n) {
      auto entry = make_entry(config[0]);
      entry->preedit = entry->text;
      cand = New<Phrase>(translator_->language(), "keylid", start_, start_ + n,
                         entry);
      cand->set_syllabifier(
          New<EnglishSyllabifier>(vector<size_t>{start_, start_ + n}));
    } else {
      cand = compose(config);
      if (!cand)
        continue;
      cand->set_type("keylid");
    }
    if (seen.insert(cand->text()).second)
      keylid_.push_back(cand);
  }
}

}  // namespace rime
