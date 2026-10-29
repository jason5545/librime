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
class UserDictionary;
struct SyllableGraph;

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
  int english_letter_trim() const { return english_letter_trim_; }
  double english_letter_penalty() const { return english_letter_penalty_; }
  int english_letter_position() const { return english_letter_position_; }
  // 英文字母的假音節（詞典裡「注音＋~」的音節，只能簡拼），拼成英文詞的碼；
  // 有字母找不到時回傳 false，code 不動（這個詞就不寫學習紀錄）
  bool EnglishCode(const string& word, Code* code);

 protected:
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
  int english_letter_trim_ = 2;
  double english_letter_penalty_ = 4.0;
  int english_letter_position_ = 6;
  bool english_letters_ready_ = false;
  vector<int32_t> english_letters_;  // a–z 的假音節；-1 表示詞典裡沒有
  // preedit_format for input after the caret (set on the context)
  an<RawInputFormat> raw_input_format_;
  the<Corrector> corrector_;
  the<Poet> poet_;
  vector<an<Phrase>> queue_;
};

}  // namespace rime

#endif  // RIME_SCRIPT_TRANSLATOR_H_
