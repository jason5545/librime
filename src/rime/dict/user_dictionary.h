//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-10-30 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_USER_DICTIONARY_H_
#define RIME_USER_DICTIONARY_H_

#include <time.h>
#include <rime/common.h>
#include <rime/component.h>
#include <rime/dict/user_db.h>
#include <rime/dict/vocabulary.h>

namespace rime {

class UserDictEntryIterator : public DictEntryFilterBinder {
 public:
  UserDictEntryIterator() = default;

  void Add(an<DictEntry>&& entry);
  void SetEntries(DictEntryList&& entries);
  void SortRange(size_t start, size_t count);

  void AddFilter(DictEntryFilter filter) override;
  an<DictEntry> Peek();
  bool Next();
  bool exhausted() const { return index_ >= cache_.size(); }
  size_t cache_size() const { return cache_.size(); }

 protected:
  bool FindNextEntry();

  DictEntryList cache_;
  size_t index_ = 0;
};

using UserDictEntryCollector = map<size_t, UserDictEntryIterator>;

class Schema;
class Table;
class Prism;
class Db;
struct SyllableGraph;
struct DfsState;
struct Ticket;

// Jason 原型：學習紀錄的排序公式（全行程共用，由 script_translator 依方案設定填入）
struct UserDictScoring {
  // 0 = 原版（新近為主）；1 = 頻率為主、新近為輔：
  //   weight = log(unit * (commits + recency * dee_now + prior_weight * prior(text)))
  int mode = 0;
  double recency = 3.0;
  double unit = 1e-5;
  double prior_weight = 0.0;
  hash_map<string, double> prior;
  // 送出後幾秒內按 BackSpace（沒有輸入中的文字時）會撤銷剛才的學習；原版寫死 3 秒
  int revert_window = 3;
};

class UserDictionary : public Class<UserDictionary, const Ticket&> {
 public:
  static UserDictScoring& scoring();

  UserDictionary(const string& name, an<Db> db);
  virtual ~UserDictionary();

  void Attach(const an<Table>& table, const an<Prism>& prism);
  bool Load();
  bool loaded() const;
  bool readonly() const;

  an<UserDictEntryCollector> Lookup(const SyllableGraph& syllable_graph,
                                    size_t start_pos,
                                    size_t depth_limit = 0,
                                    size_t predict_word_from_depth = 0,
                                    double initial_credibility = 0.0,
                                    size_t* watermark = nullptr);  // 蓬萊：讀過的最遠位置
  size_t LookupWords(UserDictEntryIterator* result,
                     const string& input,
                     bool predictive,
                     size_t limit = 0,
                     string* resume_key = NULL);
  bool UpdateEntry(const DictEntry& entry, int commits);
  // 蓬萊：學習紀錄裡這個詞（文字＋碼）的送出次數；沒有或已經刪掉回傳 false
  bool FetchCommits(const DictEntry& entry, int* commits);
  bool UpdateEntry(const DictEntry& entry,
                   int commits,
                   const string& new_entry_prefix);
  bool UpdateTickCount(TickCount increment);

  bool NewTransaction();
  bool RevertRecentTransaction();
  bool CommitPendingTransaction();

  const string& name() const { return name_; }
  TickCount tick() const { return tick_; }
  // 蓬萊：學習紀錄的版本（LevelDB 每次寫入、交易、開關都換一個新值；不是 LevelDB 時每次都不同）。
  // ScriptTranslator 跨按鍵沿用查詞結果時，版本變了就不沿用
  size_t data_version() const;

  static an<DictEntry> CreateDictEntry(const string& key,
                                       const string& value,
                                       TickCount present_tick,
                                       double credibility = 0.0,
                                       double quality_len = 0.0,
                                       string* full_code = nullptr);

 protected:
  bool Initialize();
  bool FetchTickCount();
  bool TranslateCodeToString(const Code& code, string* result);
  bool AppendSyllableToPrefix(SyllableId syllable_id,
                              const string& base,
                              string* result);
  // 蓬萊：音節編號的拼法（查過的記著；查不到是空字串）
  const string& SyllableSpelling(SyllableId syllable_id);
  // 蓬萊：音節編號由小到大是不是就是「拼法＋空白」由小到大（查一次記著）
  bool SyllablesInOrder();
  // 蓬萊：rows（學習紀錄的記憶體副本）裡第一筆 ≥「拼法＋空白」的位置，*cmp 是那一筆跟「拼法＋空白」比的結果
  //（跟 DfsLookup 的 compare_after 一樣：0 以它開頭；到底了是 1）。照副本的位址、筆數、學習紀錄版本記著
  size_t TopLowerBound(const vector<pair<string, string>>& rows,
                       size_t version,
                       SyllableId syllable_id,
                       const string& spelling,
                       int* cmp);

  void DfsLookup(const SyllableGraph& syll_graph,
                 size_t current_pos,
                 const string& current_prefix,
                 DfsState* state);

 private:
  string name_;
  an<Db> db_;
  an<Table> table_;
  an<Prism> prism_;
  hash_map<string, SyllableId> syllabary_;
  hash_map<SyllableId, string> rev_syllabary_;
  vector<const string*> spelling_by_id_;  // 蓬萊：SyllableSpelling 的快取
  int syllables_in_order_ = -1;           // 蓬萊：SyllablesInOrder 的結果（-1 還沒查）
  size_t syllable_count_ = 0;             // 蓬萊：SyllablesInOrder 查過的音節數（編號 0 到它減一）
  // 蓬萊：TopLowerBound 的快取（照音節編號；index 是 kUnknown 的還沒算）和它對應的副本、版本
  struct TopBound {
    uint32_t index;
    int32_t cmp;
  };
  vector<TopBound> top_bounds_;
  const void* top_rows_ = nullptr;
  size_t top_rows_size_ = 0;
  size_t top_version_ = 0;
  TickCount tick_ = 0;
  time_t transaction_time_ = 0;
};

class UserDictionaryComponent : public UserDictionary::Component {
 public:
  UserDictionaryComponent();
  UserDictionary* Create(const Ticket& ticket);
  UserDictionary* Create(const string& dict_name, const string& db_class);

 private:
  hash_map<string, weak<Db>> db_pool_;
};

}  // namespace rime

#endif  // RIME_USER_DICTIONARY_H_
