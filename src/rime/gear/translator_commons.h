//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2012-04-22 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_TRANSLATOR_COMMONS_H_
#define RIME_TRANSLATOR_COMMONS_H_

#include <boost/regex.hpp>
#include <rime/common.h>
#include <rime/config.h>
#include <rime/candidate.h>
#include <rime/translation.h>
#include <rime/algo/algebra.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/vocabulary.h>

namespace rime {

//

class Patterns : public vector<boost::regex> {
 public:
  bool Load(an<ConfigList> patterns);
};

//

class Spans {
 public:
  void AddVertex(size_t vertex);
  void AddSpan(size_t start, size_t end);
  void AddSpans(const Spans& spans);
  void Clear();
  // move by syllable by returning a value different from caret_pos
  size_t PreviousStop(size_t caret_pos) const;
  size_t NextStop(size_t caret_pos) const;
  size_t Count(size_t start_pos, size_t end_pos) const;
  size_t Count() const { return vertices_.empty() ? 0 : vertices_.size() - 1; }
  size_t start() const { return vertices_.empty() ? 0 : vertices_.front(); }
  size_t end() const { return vertices_.empty() ? 0 : vertices_.back(); }
  bool HasVertex(size_t vertex) const;
  void set_vertices(vector<size_t>&& vertices) { vertices_ = vertices; }

 private:
  vector<size_t> vertices_;
};

class Phrase;

class PhraseSyllabifier {
 public:
  virtual ~PhraseSyllabifier() = default;

  virtual Spans Syllabify(const Phrase* phrase) = 0;
};

//

class Language;

class Phrase : public Candidate {
 public:
  Phrase(const Language* language,
         const string& type,
         size_t start,
         size_t end,
         const an<DictEntry>& entry)
      : Candidate(type, start, end), language_(language), entry_(entry) {}
  const string& text() const { return entry_->text; }
  string comment() const { return entry_->comment; }
  string preedit() const { return entry_->preedit; }
  void set_comment(const string& comment) { entry_->comment = comment; }
  void set_preedit(const string& preedit) { entry_->preedit = preedit; }
  void set_syllabifier(an<PhraseSyllabifier> syllabifier) {
    syllabifier_ = syllabifier;
  }
  // 蓬萊：送出時數這個候選每個音節是怎麼打的（打法偵測）
  const an<PhraseSyllabifier>& syllabifier() const { return syllabifier_; }
  double weight() const { return entry_->weight; }
  void set_weight(double weight) { entry_->weight = weight; }
  // 蓬萊：按鍵模型容錯來的詞（前文重排不讓它搶走照打的第一個）
  bool correction() const { return correction_; }
  void set_correction(bool c) { correction_ = c; }
  // 蓬萊：修過的那段是沒打聲調的模糊音建議（只打首注音也可能長這樣；外掛的 Voco 重排看它）
  bool untoned_correction() const { return untoned_correction_; }
  void set_untoned_correction(bool c) { untoned_correction_ = c; }
  Code& code() const { return entry_->code; }
  const DictEntry& entry() const { return *entry_; }
  const Language* language() const { return language_; }
  size_t matching_code_size() const {
    return entry_->matching_code_size != 0 ? entry_->matching_code_size
                                           : entry_->code.size();
  }
  bool is_exact_match() const {
    return entry_->matching_code_size == 0 ||
           entry_->matching_code_size == entry_->code.size();
  }
  bool is_predicitve_match() const {
    return entry_->matching_code_size != 0 &&
           entry_->matching_code_size < entry_->code.size();
  }
  Code matching_code() const {
    return entry_->matching_code_size == 0
               ? entry_->code
               : Code(entry_->code.begin(),
                      entry_->code.begin() + entry_->matching_code_size);
  }
  Spans spans() {
    return syllabifier_ ? syllabifier_->Syllabify(this) : Spans();
  }

 protected:
  const Language* language_;
  an<DictEntry> entry_;
  an<PhraseSyllabifier> syllabifier_;
  bool correction_ = false;
  bool untoned_correction_ = false;
};

//

class Sentence : public Phrase {
 public:
  Sentence(const Language* language)
      : Phrase(language, "sentence", 0, 0, New<DictEntry>()) {}
  Sentence(const Sentence& other)
      : Phrase(other),
        components_(other.components_),
        word_lengths_(other.word_lengths_) {
    entry_ = New<DictEntry>(other.entry());
  }
  void Extend(const DictEntry& another, size_t end_pos, double new_weight);
  void Offset(size_t offset);

  bool empty() const { return components_.empty(); }

  size_t size() const { return components_.size(); }

  const vector<DictEntry>& components() const { return components_; }
  const vector<size_t>& word_lengths() const { return word_lengths_; }

 protected:
  vector<DictEntry> components_;
  vector<size_t> word_lengths_;
};

//

struct Ticket;

// 蓬萊：使用者資料夾裡的隱藏詞清單（translator/dictionary_exclude_file），一行一個詞，
// 詞後面可以接 Tab 和註記，# 開頭是註解。同一個檔案所有 session 共用；
// 檔案大小或修改時間變了就重讀，手改清單不用重新部署
class ExcludedWords {
 public:
  explicit ExcludedWords(const path& file) : file_(file) {}
  static an<ExcludedWords> Require(const path& file);

  // 檔案變了就重讀
  void Refresh();
  // 加進清單並寫進檔案（詞<Tab>時間）
  bool Add(const string& word);
  bool Has(const string& word) const { return words_.count(word) > 0; }
  const hash_set<string>& words() const { return words_; }
  // 清單內容每變一次加一
  int version() const { return version_; }

 private:
  void Load();

  path file_;
  hash_set<string> words_;
  bool checked_ = false;
  bool exists_ = false;
  uintmax_t size_ = 0;
  std::filesystem::file_time_type mtime_;
  int version_ = 0;
};

class TranslatorOptions {
 public:
  TranslatorOptions(const Ticket& ticket);
  bool IsUserDictDisabledFor(const string& input) const;

  const string& delimiters() const { return delimiters_; }
  vector<string> tags() const { return tags_; }
  void set_tags(const vector<string>& tags) {
    tags_ = tags;
    if (tags_.size() == 0) {
      tags_.push_back("abc");
    }
  }
  const string& tag() const { return tags_[0]; }
  void set_tag(const string& tag) { tags_[0] = tag; }
  bool contextual_suggestions() const { return contextual_suggestions_; }
  void set_contextual_suggestions(bool enabled) {
    contextual_suggestions_ = enabled;
  }
  bool enable_completion() const { return enable_completion_; }
  void set_enable_completion(bool enabled) { enable_completion_ = enabled; }
  bool strict_spelling() const { return strict_spelling_; }
  void set_strict_spelling(bool is_strict) { strict_spelling_ = is_strict; }
  double initial_quality() const { return initial_quality_; }
  void set_initial_quality(double quality) { initial_quality_ = quality; }
  Projection& preedit_formatter() { return preedit_formatter_; }
  Projection& comment_formatter() { return comment_formatter_; }
  const hash_set<string>& blacklist() { return blacklist_; }
  // 蓬萊：隱藏詞清單有變就併進 blacklist（每次查詢前呼叫）；沒設 dictionary_exclude_file 時什麼都不做
  void RefreshBlacklist();
  ExcludedWords* excluded_words() const { return excluded_words_.get(); }
  // 蓬萊：blacklist 換過幾次（隱藏詞清單重新載入就變）
  int blacklist_version() const { return excluded_version_; }
  int dictionary_exclude_min_length() const {
    return dictionary_exclude_min_length_;
  }

 protected:
  string delimiters_;
  vector<string> tags_{"abc"};  // invariant: non-empty
  bool contextual_suggestions_ = false;
  // 蓬萊：新輸入開頭取前文時，跳過送出紀錄尾巴的空段落（Return 送出時留下的空字串 raw），
  // 接到上一筆送出的文字；false＝原版（拿到空字串，等於沒有前文）
  bool last_commit_as_context_ = true;
  bool enable_completion_ = true;
  bool strict_spelling_ = false;
  double initial_quality_ = 0.;
  int max_sentences_ = 1;
  double sentence_cutoff_threshold_ = 0.1;
  Projection preedit_formatter_;
  Projection comment_formatter_;
  Patterns user_dict_disabling_patterns_;
  hash_set<string> blacklist_;
  // 蓬萊：隱藏詞清單
  hash_set<string> config_blacklist_;  // dictionary_exclude 原本的詞
  an<ExcludedWords> excluded_words_;
  int excluded_version_ = -1;
  int dictionary_exclude_min_length_ = 2;
};

}  // namespace rime

#endif  // RIME_TRANSLATOR_COMMONS_H_
