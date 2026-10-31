//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-08-07 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_SCRIPT_TRANSLATOR_H_
#define RIME_SCRIPT_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translation.h>
#include <rime/translator.h>
#include <rime/algo/algebra.h>
#include <rime/gear/english_keylid.h>
#include <rime/gear/memory.h>
#include <rime/gear/translator_commons.h>

namespace rime {

class Code;
class Corrector;
struct DictEntry;
class Dictionary;
class EnglishJudge;
class EnglishLexicon;
class Poet;
struct RawInputFormat;
struct SentenceCache;
class UserDictionary;
struct SyllableGraph;

// 蓬萊：同一段輸入兩種切法比整句分數時，換成另一種切法要多贏的分數（log 機率）。選字時吃幾個鍵
// （translator/extend_selection，docs/optimization.md 第 4 項第 6 條）用它；translator/split_penalty 可以改
constexpr double kSplitPenalty = 0.0;

class ScriptTranslator : public Translator,
                         public Memory,
                         public TranslatorOptions {
 public:
  ScriptTranslator(const Ticket& ticket);
  virtual ~ScriptTranslator();

  virtual an<Translation> Query(const string& input,
                                const Segment& segment) override;
  virtual bool Memorize(const CommitEntry& commit_entry) override;
  virtual bool ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                      const Segment& seg) override;

  string FormatPreedit(const string& preedit);
  string Spell(const Code& code);
  string GetPrecedingText(size_t start) const;
  bool UpdateElements(const CommitEntry& commit_entry);

  bool ConcatenatePhrases(CommitEntry& commit_entry,
                          const vector<an<Phrase>>& phrases);
  bool SaveCommitEntry(CommitEntry& commit_entry);

  // options
  int max_homophones() const { return max_homophones_; }
  int spelling_hints() const { return spelling_hints_; }
  bool always_show_comments() const { return always_show_comments_; }
  bool enable_word_completion() const { return enable_word_completion_; }
  int max_word_length() const { return max_word_length_; }
  int core_word_length() const;
  int max_user_phrase_syllables() const { return max_user_phrase_syllables_; }
  double split_penalty() const { return split_penalty_; }
  // 蓬萊：候選排序（docs/optimization.md 第 4 項第 1 條）
  int long_phrase_quota() const { return long_phrase_quota_; }
  int recent_phrase_max() const { return recent_phrase_max_; }
  int recent_phrase_window() const { return recent_phrase_window_; }
  // 蓬萊：跨按鍵沿用的整句材料（script_translator.cc 的 SentenceCache）。帶上這次查詞用的詞典、學習紀錄，
  // 跟上次不同（讀音變體、隱藏詞清單、學習紀錄寫入）就整個清掉
  SentenceCache* sentence_cache(Dictionary* dict, UserDictionary* user_dict);
  // 蓬萊：英文單字進整句
  const an<EnglishLexicon>& english_lexicon() const { return english_lexicon_; }
  int english_min_length() const { return english_min_length_; }
  double english_min_zipf() const { return english_min_zipf_; }
  int english_position() const { return english_position_; }
  int english_max_sentences() const { return english_max_sentences_; }
  double english_weight() const { return english_weight_; }
  // 蓬萊：中英混輸的判斷（translator/english_judge）。有表時英文句和原本的第一候選都打分，
  // 英文句贏過第一候選 english_judge_tau × 英文段數以上才排第一；沒有表時照舊排在 english_position
  const an<EnglishJudge>& english_judge() const { return english_judge_; }
  double english_judge_tau() const { return english_judge_tau_; }
  int english_judge_learned() const { return english_judge_learned_; }
  bool english_letter_runs() const { return english_letter_runs_; }
  bool english_space() const { return english_space_; }
  // 後面緊接空白的單字表詞最短幾個字母（沒設時跟 english_min_length 一樣）
  int english_space_min_length() const {
    return english_space_min_length_ > 0 ? english_space_min_length_
                                         : english_min_length_;
  }
  // 單字表照這個長度載入：english_space 打開時取兩者較短的
  int english_lexicon_min_length() const {
    return english_space_ ? (std::min)(english_min_length_,
                                       english_space_min_length())
                          : english_min_length_;
  }
  int english_letter_trim() const { return english_letter_trim_; }
  double english_letter_penalty() const { return english_letter_penalty_; }
  int english_letter_position() const { return english_letter_position_; }
  // 中文那邊比幾個讀法：原本的第一候選＋前 english_judge_readings 個整句，取判斷分數最高的
  int english_judge_readings() const { return english_judge_readings_; }
  // 整段都是數字鍵時也試數字（退回按鍵原本的字）：至少 digit_min_length 鍵，比照字母串多扣 digit_penalty
  bool digit_runs() const { return digit_runs_; }
  int digit_min_length() const { return digit_min_length_; }
  double digit_penalty() const { return digit_penalty_; }
  // 按鍵層語言辨識（translator/english_keylid）：中文注音鍵模型對英文字母模型找出單字表以外的英文段，另外組成候選，
  // 排在第 english_keylid_position 個起、最多 english_keylid_max 個（原本前面的候選不動、不排第一）。要有判斷表才有作用
  const an<EnglishKeylid>& english_keylid() const { return english_keylid_; }
  const EnglishKeylid::Params& keylid_params() const { return keylid_params_; }
  int english_keylid_candidates() const { return english_keylid_candidates_; }
  int english_keylid_max() const { return english_keylid_max_; }
  // 數字串裡收了連著的聲調鍵（speller/digit_tones）：中文切不開整段，數字直接排第一
  bool HasDigitTonePair(const string& input) const;
  // 開頭是聲調鍵的數字（speller/digit_lead_tones）：出得了數字候選時，開頭聲調鍵那段 raw 加標籤給 abc_segmentor 看；
  // 收成一段之後只給數字候選
  bool DigitLeadReady() const;
  an<Translation> TranslateDigitLead(const string& input,
                                     const Segment& segment);
  // 開頭是 translator/raw_lead_keys 裡的鍵（/，ㄥ 開頭的音節只有「鞥」）、第二鍵不是聲調鍵：
  // 組不成中文，只給按鍵原字（斜線指令 /loop）
  bool RawLead(const string& input) const;
  an<Translation> TranslateRawLead(const string& input, const Segment& segment);
  // 英文字母的假音節（詞典裡「注音＋~」的音節，只能簡拼），拼成英文詞的碼；
  // 有字母找不到時回傳 false，code 不動（這個詞就不寫學習紀錄）
  bool EnglishCode(const string& word, Code* code);

 protected:
  // 蓬萊：詞典的變體表跟著方案選項切換（選項名稱＝變體名稱，都沒打開就用主表）
  void SelectDictionaryVariant();
  vector<string> variant_options_;  // 方案 switches 裡所有的選項名稱
  // 蓬萊：沒有學習紀錄的詞典詞（兩個字以上）刪除時改成隱藏（translator/dictionary_exclude_file）
  bool ExcludeOnDelete(const an<Phrase>& phrase) override;
  // 蓬萊：忘掉上一筆（translator/forget_last_code）
  struct Written {
    string text;
    Code code;
    bool whole;  // true：送出的整筆（送出次數 +1）；false：整句裡的詞（送出次數 +0）
  };
  struct Remembered {
    string text;  // 實際送出的文字
    vector<Written> written;
  };
  void WriteUserEntry(const DictEntry& entry, int commits);
  deque<Remembered>& forget_history();
  bool ForgetSelected(Context* ctx) const;
  void OnSelectForForget(Context* ctx);
  void OnCommitForForget(Context* ctx);
  void ForgetLast(Context* ctx);
  string forget_last_code_;
  int forget_last_history_ = 10;
  vector<Written> pending_;  // 這次送出寫進學習紀錄的
  connection forget_select_connection_;
  connection forget_commit_connection_;
  // 蓬萊：選字時引擎決定選到的詞吃幾個鍵（translator/extend_selection）。選到的詞最後一個字的讀音還能往後接
  // （簡拼的 ㄋ 跟 ㄋㄧ），就兩種吃法各算一次「這個詞＋剩下的最佳整句」，多吃的贏 split_penalty 以上才換。
  // 比引擎的 OnSelect 早跑（它照選到的候選切段落）。算過幾次、換了幾次記在 context 的 penglai_extend（測試看的）
  void OnSelectExtend(Context* ctx);
  bool extend_selection_ = false;
  double split_penalty_ = kSplitPenalty;
  int extend_evaluated_ = 0;
  int extend_taken_ = 0;
  connection extend_select_connection_;
  // 蓬萊：候選排序（docs/optimization.md 第 4 項第 1 條；0 都是照原版）。
  // long_phrase_quota：蓋不住整段的詞裡，三個音節以上的每種長度先出幾個，其餘排到二字、一字後面。
  // recent_phrase_max：學習紀錄裡最近打過的詞（兩個音節以上、蓋不住整段）最多幾個拉到整句、整段剛好的詞後面；
  // recent_phrase_window：「最近」是學習紀錄最近幾次送出以內（0＝不限，照最後一次打的先後挑）
  int long_phrase_quota_ = 0;
  int recent_phrase_max_ = 0;
  int recent_phrase_window_ = 0;

  int max_homophones_ = 1;
  int spelling_hints_ = 0;
  int max_word_length_ = 0;
  int core_word_length_ = 0;
  bool always_show_comments_ = false;
  bool enable_correction_ = false;
  bool enable_word_completion_ = false;
  // Jason 原型：整句可用的學習紀錄最長音節數（原版寫死 5）
  int max_user_phrase_syllables_ = 5;
  // 蓬萊：英文單字進整句（translator/english_lexicon 沒設就不啟用）
  an<EnglishLexicon> english_lexicon_;
  int english_min_length_ = 4;
  double english_min_zipf_ = 3.0;
  int english_position_ = 2;
  int english_max_sentences_ = 2;
  double english_weight_ = 0.0;
  an<EnglishJudge> english_judge_;
  double english_judge_tau_ = 4.0;
  int english_judge_learned_ = 4;
  bool english_letter_runs_ = false;
  // 英文後面緊接的空白（一聲）算進英文那段，送出時留著空白
  bool english_space_ = false;
  int english_space_min_length_ = 0;
  int english_letter_trim_ = 2;
  double english_letter_penalty_ = 4.0;
  int english_letter_position_ = 6;
  int english_judge_readings_ = 5;
  bool digit_runs_ = false;
  int digit_min_length_ = 2;
  double digit_penalty_ = 4.0;
  bool digit_tones_ = false;
  bool digit_lead_tones_ = false;
  string raw_lead_keys_;
  an<EnglishKeylid> english_keylid_;
  double english_keylid_switch_ = 4.0;
  int english_keylid_min_length_ = 2;
  double english_keylid_margin_ = 8.0;
  int english_keylid_beam_ = 64;
  EnglishKeylid::Params keylid_params_;
  int english_keylid_candidates_ = 8;
  int english_keylid_position_ = 6;
  int english_keylid_max_ = 4;
  int page_size_ = 5;
  string speller_finals_;
  bool english_letters_ready_ = false;
  vector<int32_t> english_letters_;  // a–z 的假音節；-1 表示詞典裡沒有
  // preedit_format for input after the caret (set on the context)
  an<RawInputFormat> raw_input_format_;
  the<SentenceCache> sentence_cache_;
  the<Corrector> corrector_;
  the<Poet> poet_;
  vector<an<Phrase>> queue_;
};

}  // namespace rime

#endif  // RIME_SCRIPT_TRANSLATOR_H_
