//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-01 GONG Chen <chen.sst@gmail.com>
//

#ifndef RIME_TABLE_H_
#define RIME_TABLE_H_

#include <cstring>
#include <algorithm>
#include <rime/common.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/vocabulary.h>
#include <rime/dict/string_table.h>

#define RIME_TABLE_UNION(U, V, A, a, B, b)      \
  struct U {                                    \
    V value;                                    \
    const A& a() const {                        \
      return *reinterpret_cast<const A*>(this); \
    }                                           \
    const B& b() const {                        \
      return *reinterpret_cast<const B*>(this); \
    }                                           \
    A& a() {                                    \
      return *reinterpret_cast<A*>(this);       \
    }                                           \
    B& b() {                                    \
      return *reinterpret_cast<B*>(this);       \
    }                                           \
  }

namespace rime {

namespace table {

// union StringType {
//   String str;
//   StringId str_id;
// };
RIME_TABLE_UNION(StringType, int32_t, String, str, StringId, str_id);

using Syllabary = Array<StringType>;

using Code = List<SyllableId>;

using Weight = float;

struct Entry {
  StringType text;
  Weight weight;
};

struct LongEntry {
  Code extra_code;
  Entry entry;
};

struct PhraseIndex;

struct HeadIndexNode {
  List<Entry> entries;
  OffsetPtr<PhraseIndex> next_level;
};

using HeadIndex = Array<HeadIndexNode>;

struct TrunkIndexNode {
  SyllableId key;
  List<Entry> entries;
  OffsetPtr<PhraseIndex> next_level;
};

using TrunkIndex = Array<TrunkIndexNode>;

using TailIndex = Array<LongEntry>;

// union PhraseIndex {
//   TrunkIndex trunk;
//   TailIndex tail;
// };
RIME_TABLE_UNION(PhraseIndex, Array<char>, TrunkIndex, trunk, TailIndex, tail);

using Index = HeadIndex;

struct Metadata {
  static const int kFormatMaxLength = 32;
  char format[kFormatMaxLength];
  uint32_t dict_file_checksum;
  uint32_t num_syllables;
  uint32_t num_entries;
  OffsetPtr<Syllabary> syllabary;
  OffsetPtr<Index> index;
  // v2
  int32_t reserved_1;
  int32_t reserved_2;
  OffsetPtr<char> string_table;
  uint32_t string_table_size;
};

}  // namespace table

// 蓬萊：查詞典時的碼。索引碼最多 Code::kIndexCodeMaxLength 個音節，TableAccessor、Chunk 一個非聲調鍵
// 要建幾百個，碼放在物件裡、不另外配置；比這長的（帶 extra_code 的長詞）才放進 vector
class SmallCode {
 public:
  SmallCode() = default;
  SmallCode(const SyllableId* code, size_t size) { Assign(code, size); }
  explicit SmallCode(const Code& code) { Assign(code.data(), code.size()); }
  void Assign(const SyllableId* code, size_t size) {
    size_ = size;
    if (size <= kInline) {
      std::copy(code, code + size, inline_);
      long_.clear();
    } else {
      long_.assign(code, code + size);
    }
  }
  size_t size() const { return size_; }
  const SyllableId* data() const {
    return size_ <= kInline ? inline_ : long_.data();
  }
  Code ToCode() const {
    Code code;
    code.assign(data(), data() + size_);
    return code;
  }

 private:
  static constexpr size_t kInline = Code::kIndexCodeMaxLength;
  SyllableId inline_[kInline] = {};
  size_t size_ = 0;
  vector<SyllableId> long_;
};

class TableAccessor {
 public:
  TableAccessor() = default;
  TableAccessor(const Code& index_code,
                const List<table::Entry>* entries,
                double credibility = 0.0,
                double quality_len = 0.0);
  // 蓬萊：Table::Query 用，碼是陣列（SmallCode，不配置）
  TableAccessor(const SyllableId* index_code,
                size_t index_code_size,
                const List<table::Entry>* entries,
                double credibility,
                double quality_len);
  TableAccessor(const SyllableId* index_code,
                size_t index_code_size,
                const table::TailIndex* code_map,
                double credibility,
                double quality_len);
  TableAccessor(const Code& index_code,
                const Array<table::Entry>* entries,
                double credibility = 0.0,
                double quality_len = 0.0);
  TableAccessor(const Code& index_code,
                const table::TailIndex* code_map,
                double credibility = 0.0,
                double quality_len = 0.0);

  RIME_DLL bool Next();

  RIME_DLL bool exhausted() const;
  RIME_DLL size_t remaining() const;
  RIME_DLL const table::Entry* entry() const;
  RIME_DLL const table::Code* extra_code() const;
  // 蓬萊：碼放在 SmallCode 裡，這裡組一份 Code（原本回傳參照）
  Code index_code() const { return index_code_.ToCode(); }
  const SmallCode& small_index_code() const { return index_code_; }
  size_t index_code_size() const { return index_code_.size(); }
  Code code() const;
  double credibility() const { return credibility_; }
  double quality_len() const { return quality_len_; }

 private:
  SmallCode index_code_;
  const table::Entry* entries_ = nullptr;
  const table::LongEntry* long_entries_ = nullptr;
  size_t size_ = 0;
  size_t cursor_ = 0;
  double credibility_ = 0.0;
  double quality_len_ = 0.0;
};

using TableQueryResult = map<int, vector<TableAccessor>>;

struct SyllableGraph;

class TableQuery {
 public:
  TableQuery(table::Index* index) : lv1_index_(index) { Reset(); }

  TableAccessor Access(SyllableId syllable_id,
                       double credibility = 0.0,
                       double quality_len = 0.0) const;

  // down to next level
  bool Advance(SyllableId syllable_id,
               double credibility = 0.0,
               double quality_len = 0.0,
               size_t last_pos = 0);

  // up one level
  bool Backdate();

  // back to root
  void Reset();

  size_t level() const { return level_; }

  double credibility_sum() const {
    return credibility_.empty() ? 0 : credibility_.back();
  }
  double quality_len_sum() const {
    return quality_len_.empty() ? 0 : quality_len_.back();
  }
  size_t last_pos() const { return last_pos_.empty() ? 0 : last_pos_.back(); }

 protected:
  size_t level_ = 0;
  Code index_code_;
  vector<double> credibility_;
  vector<double> quality_len_;
  vector<size_t> last_pos_;

 private:
  bool Walk(SyllableId syllable_id);

  table::HeadIndex* lv1_index_ = nullptr;
  table::TrunkIndex* lv2_index_ = nullptr;
  table::TrunkIndex* lv3_index_ = nullptr;
  table::TailIndex* lv4_index_ = nullptr;
};

class Table : public MappedFile {
 public:
  RIME_DLL Table(const path& file_path);
  virtual ~Table();

  RIME_DLL bool Load();
  RIME_DLL bool Save();
  RIME_DLL bool Build(const Syllabary& syllabary,
                      const Vocabulary& vocabulary,
                      size_t num_entries,
                      uint32_t dict_file_checksum = 0);

  bool GetSyllabary(Syllabary* syllabary);
  RIME_DLL string GetSyllableById(int syllable_id);
  RIME_DLL TableAccessor QueryWords(int syllable_id);
  RIME_DLL TableAccessor QueryPhrases(const Code& code);
  // 蓬萊：watermark 不是 nullptr 時記下查詢讀過的最遠位置（音節圖的頂點），ScriptTranslator 跨按鍵沿用查詞結果用
  RIME_DLL bool Query(const SyllableGraph& syll_graph,
                      size_t start_pos,
                      TableQueryResult* result,
                      size_t* watermark = nullptr);
  RIME_DLL string GetEntryText(const table::Entry& entry);

  uint32_t dict_file_checksum() const;
  table::Metadata* metadata() const { return metadata_; }

 private:
  table::Index* BuildIndex(const Vocabulary& vocabulary, size_t num_syllables);
  table::HeadIndex* BuildHeadIndex(const Vocabulary& vocabulary,
                                   size_t num_syllables);
  table::TrunkIndex* BuildTrunkIndex(const Code& prefix,
                                     const Vocabulary& vocabulary);
  table::TailIndex* BuildTailIndex(const Code& prefix,
                                   const Vocabulary& vocabulary);
  bool BuildPhraseIndex(Code code,
                        const Vocabulary& vocabulary,
                        map<string, int>* index_data);
  Array<table::Entry>* BuildEntryArray(const ShortDictEntryList& entries);
  bool BuildEntryList(const ShortDictEntryList& src, List<table::Entry>* dest);
  bool BuildEntry(const ShortDictEntry& dict_entry, table::Entry* entry);

  string GetString(const table::StringType& x);
  bool AddString(const string& src, table::StringType* dest, double weight);
  bool OnBuildStart();
  bool OnBuildFinish();
  bool OnLoad();
  void Unload();

 protected:
  table::Metadata* metadata_ = nullptr;
  table::Syllabary* syllabary_ = nullptr;
  table::Index* index_ = nullptr;

  the<StringTable> string_table_;
  the<StringTableBuilder> string_table_builder_;
};

}  // namespace rime

#endif  // RIME_TABLE_H_
