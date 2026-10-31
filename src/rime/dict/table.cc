//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-02 GONG Chen <chen.sst@gmail.com>
//
#include <cfloat>
#include <cstring>
#include <algorithm>
#include <queue>
#include <utility>
#include <rime/common.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/table.h>

namespace rime {

const char kTableFormatLatest[] = "Rime::Table/4.0";
const int kTableFormatLowestCompatible = 4.0;

const char kTableFormatPrefix[] = "Rime::Table/";
const size_t kTableFormatPrefixLen = sizeof(kTableFormatPrefix) - 1;

TableAccessor::TableAccessor(const Code& index_code,
                             const List<table::Entry>* list,
                             double credibility,
                             double quality_len)
    : index_code_(index_code),
      entries_(list->at.get()),
      size_(list->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

TableAccessor::TableAccessor(const SyllableId* index_code,
                             size_t index_code_size,
                             const List<table::Entry>* list,
                             double credibility,
                             double quality_len)
    : index_code_(index_code, index_code_size),
      entries_(list->at.get()),
      size_(list->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

TableAccessor::TableAccessor(const SyllableId* index_code,
                             size_t index_code_size,
                             const table::TailIndex* code_map,
                             double credibility,
                             double quality_len)
    : index_code_(index_code, index_code_size),
      long_entries_(code_map->at),
      size_(code_map->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

TableAccessor::TableAccessor(const Code& index_code,
                             const Array<table::Entry>* array,
                             double credibility,
                             double quality_len)
    : index_code_(index_code),
      entries_(array->at),
      size_(array->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

TableAccessor::TableAccessor(const Code& index_code,
                             const table::TailIndex* code_map,
                             double credibility,
                             double quality_len)
    : index_code_(index_code),
      long_entries_(code_map->at),
      size_(code_map->size),
      credibility_(credibility),
      quality_len_(quality_len) {}

bool TableAccessor::exhausted() const {
  if (entries_ || long_entries_) {
    return !(size_ - cursor_);
  }
  return true;
}

size_t TableAccessor::remaining() const {
  if (entries_ || long_entries_) {
    return size_ - cursor_;
  }
  return 0;
}

const table::Entry* TableAccessor::entry() const {
  if (exhausted())
    return NULL;
  if (entries_)
    return &entries_[cursor_];
  else
    return &long_entries_[cursor_].entry;
}

const table::Code* TableAccessor::extra_code() const {
  if (!long_entries_ || cursor_ >= size_)
    return NULL;
  return &long_entries_[cursor_].extra_code;
}

Code TableAccessor::code() const {
  auto extra = extra_code();
  if (!extra) {
    return index_code();
  }
  Code code(index_code());
  for (auto p = extra->begin(); p != extra->end(); ++p) {
    code.push_back(*p);
  }
  return code;
}

bool TableAccessor::Next() {
  if (exhausted())
    return false;
  ++cursor_;
  return !exhausted();
}

bool TableQuery::Advance(SyllableId syllable_id,
                         double credibility,
                         double quality_len,
                         size_t last_pos) {
  if (!Walk(syllable_id)) {
    return false;
  }
  ++level_;
  index_code_.push_back(syllable_id);
  credibility_.push_back(credibility_sum() + credibility);
  quality_len_.push_back(quality_len_sum() + quality_len);
  last_pos_.push_back(last_pos);
  return true;
}

bool TableQuery::Backdate() {
  if (level_ == 0)
    return false;
  --level_;
  if (index_code_.size() > level_) {
    index_code_.pop_back();
    credibility_.pop_back();
    quality_len_.pop_back();
    last_pos_.pop_back();
  }
  return true;
}

void TableQuery::Reset() {
  level_ = 0;
  index_code_.clear();
  credibility_.clear();
  quality_len_.clear();
  last_pos_.clear();
}

inline static bool node_less(const table::TrunkIndexNode& a,
                             const table::TrunkIndexNode& b) {
  return a.key < b.key;
}

static table::TrunkIndexNode* find_node(table::TrunkIndexNode* first,
                                        table::TrunkIndexNode* last,
                                        const SyllableId& key) {
  table::TrunkIndexNode target;
  target.key = key;
  auto it = std::lower_bound(first, last, target, node_less);
  return it == last || key < it->key ? last : it;
}

bool TableQuery::Walk(SyllableId syllable_id) {
  if (level_ == 0) {
    if (!lv1_index_ || syllable_id < 0 ||
        syllable_id >= static_cast<SyllableId>(lv1_index_->size))
      return false;
    auto node = &lv1_index_->at[syllable_id];
    if (!node->next_level)
      return false;
    lv2_index_ = &node->next_level->trunk();
  } else if (level_ == 1) {
    if (!lv2_index_)
      return false;
    auto node = find_node(lv2_index_->begin(), lv2_index_->end(), syllable_id);
    if (node == lv2_index_->end())
      return false;
    if (!node->next_level)
      return false;
    lv3_index_ = &node->next_level->trunk();
  } else if (level_ == 2) {
    if (!lv3_index_)
      return false;
    auto node = find_node(lv3_index_->begin(), lv3_index_->end(), syllable_id);
    if (node == lv3_index_->end())
      return false;
    if (!node->next_level)
      return false;
    lv4_index_ = &node->next_level->tail();
  } else {
    return false;
  }
  return true;
}

inline static Code add_syllable(Code code, SyllableId syllable_id) {
  code.push_back(syllable_id);
  return code;
}

TableAccessor TableQuery::Access(SyllableId syllable_id,
                                 double credibility,
                                 double quality_len) const {
  credibility += credibility_sum();
  quality_len += quality_len_sum();
  if (level_ == 0) {
    if (!lv1_index_ || syllable_id < 0 ||
        syllable_id >= static_cast<SyllableId>(lv1_index_->size))
      return TableAccessor();
    auto node = &lv1_index_->at[syllable_id];
    return TableAccessor(add_syllable(index_code_, syllable_id), &node->entries,
                         credibility, quality_len);
  } else if (level_ == 1 || level_ == 2) {
    auto index = (level_ == 1) ? lv2_index_ : lv3_index_;
    if (!index)
      return TableAccessor();
    auto node = find_node(index->begin(), index->end(), syllable_id);
    if (node == index->end())
      return TableAccessor();
    return TableAccessor(add_syllable(index_code_, syllable_id), &node->entries,
                         credibility, quality_len);
  } else if (level_ == 3) {
    if (!lv4_index_)
      return TableAccessor();
    return TableAccessor(index_code_, lv4_index_, credibility, quality_len);
  }
  return TableAccessor();
}

// string Table::GetString_v1(const table::StringType& x) {
//  return x.str().c_str();
// }

// bool Table::AddString_v1(const string& src, table::StringType* dest,
//                          double /*weight*/) {
//   return CopyString(src, &dest->str());
// }

string Table::GetString(const table::StringType& x) {
  return string_table_->GetString(x.str_id());
}

bool Table::AddString(const string& src,
                      table::StringType* dest,
                      double weight) {
  string_table_builder_->Add(src, weight, &dest->str_id());
  return true;
}

bool Table::OnBuildStart() {
  string_table_builder_.reset(new StringTableBuilder);
  return true;
}

bool Table::OnBuildFinish() {
  string_table_builder_->Build();
  // saving string table image
  size_t image_size = string_table_builder_->BinarySize();
  char* image = Allocate<char>(image_size);
  if (!image) {
    LOG(ERROR) << "Error creating string table image.";
    return false;
  }
  string_table_builder_->Dump(image, image_size);
  metadata_->string_table = image;
  metadata_->string_table_size = image_size;
  return true;
}

bool Table::OnLoad() {
  string_table_.reset(new StringTable(metadata_->string_table.get(),
                                      metadata_->string_table_size));
  return true;
}

Table::Table(const path& file_path) : MappedFile(file_path) {}

Table::~Table() {}

bool Table::Load() {
  LOG(INFO) << "loading table file: " << file_path();

  if (IsOpen())
    Close();

  if (!OpenReadOnly()) {
    LOG(ERROR) << "Error opening table file '" << file_path() << "'.";
    return false;
  }

  metadata_ = Find<table::Metadata>(0);
  if (!metadata_) {
    LOG(ERROR) << "metadata not found.";
    Close();
    return false;
  }
  if (strncmp(metadata_->format, kTableFormatPrefix, kTableFormatPrefixLen)) {
    LOG(ERROR) << "invalid metadata.";
    Close();
    return false;
  }
  double format_version = atof(&metadata_->format[kTableFormatPrefixLen]);
  if (format_version < kTableFormatLowestCompatible - DBL_EPSILON) {
    LOG(ERROR) << "table format version " << format_version
               << " is no longer supported. please upgrade to version "
               << kTableFormatLatest;
    return false;
  }

  syllabary_ = metadata_->syllabary.get();
  if (!syllabary_) {
    LOG(ERROR) << "syllabary not found.";
    Close();
    return false;
  }
  index_ = metadata_->index.get();
  if (!index_) {
    LOG(ERROR) << "table index not found.";
    Close();
    return false;
  }

  return OnLoad();
}

bool Table::Save() {
  LOG(INFO) << "saving table file: " << file_path();

  if (!index_) {
    LOG(ERROR) << "the table has not been constructed!";
    return false;
  }

  return ShrinkToFit();
}

uint32_t Table::dict_file_checksum() const {
  return metadata_ ? metadata_->dict_file_checksum : 0;
}

bool Table::Build(const Syllabary& syllabary,
                  const Vocabulary& vocabulary,
                  size_t num_entries,
                  uint32_t dict_file_checksum) {
  const size_t kReservedSize = 4096;
  size_t num_syllables = syllabary.size();
  size_t estimated_file_size =
      kReservedSize + 32 * num_syllables + 64 * num_entries;
  LOG(INFO) << "building table.";
  LOG(INFO) << "num syllables: " << num_syllables;
  LOG(INFO) << "num entries: " << num_entries;
  LOG(INFO) << "estimated file size: " << estimated_file_size;
  if (!Create(estimated_file_size)) {
    LOG(ERROR) << "Error creating table file '" << file_path() << "'.";
    return false;
  }

  LOG(INFO) << "creating metadata.";
  metadata_ = Allocate<table::Metadata>();
  if (!metadata_) {
    LOG(ERROR) << "Error creating metadata in file '" << file_path() << "'.";
    return false;
  }
  metadata_->dict_file_checksum = dict_file_checksum;
  metadata_->num_syllables = num_syllables;
  metadata_->num_entries = num_entries;

  if (!OnBuildStart()) {
    return false;
  }

  LOG(INFO) << "creating syllabary.";
  syllabary_ = CreateArray<table::StringType>(num_syllables);
  if (!syllabary_) {
    LOG(ERROR) << "Error creating syllabary.";
    return false;
  } else {
    size_t i = 0;
    for (const string& syllable : syllabary) {
      AddString(syllable, &syllabary_->at[i++], 0.0);
    }
  }
  metadata_->syllabary = syllabary_;

  LOG(INFO) << "creating table index.";
  index_ = BuildIndex(vocabulary, num_syllables);
  if (!index_) {
    LOG(ERROR) << "Error creating table index.";
    return false;
  }
  metadata_->index = index_;

  if (!OnBuildFinish()) {
    return false;
  }

  // at last, complete the metadata
  std::strncpy(metadata_->format, kTableFormatLatest,
               table::Metadata::kFormatMaxLength);
  return true;
}

table::Index* Table::BuildIndex(const Vocabulary& vocabulary,
                                size_t num_syllables) {
  return reinterpret_cast<table::Index*>(
      BuildHeadIndex(vocabulary, num_syllables));
}

table::HeadIndex* Table::BuildHeadIndex(const Vocabulary& vocabulary,
                                        size_t num_syllables) {
  auto index = CreateArray<table::HeadIndexNode>(num_syllables);
  if (!index) {
    return NULL;
  }
  for (const auto& v : vocabulary) {
    int syllable_id = v.first;
    auto& node(index->at[syllable_id]);
    const auto& entries(v.second.entries);
    if (!BuildEntryList(entries, &node.entries)) {
      return NULL;
    }
    if (v.second.next_level) {
      Code code;
      code.push_back(syllable_id);
      auto next_level_index = BuildTrunkIndex(code, *v.second.next_level);
      if (!next_level_index) {
        return NULL;
      }
      node.next_level = reinterpret_cast<table::PhraseIndex*>(next_level_index);
    }
  }
  return index;
}

table::TrunkIndex* Table::BuildTrunkIndex(const Code& prefix,
                                          const Vocabulary& vocabulary) {
  auto index = CreateArray<table::TrunkIndexNode>(vocabulary.size());
  if (!index) {
    return NULL;
  }
  size_t count = 0;
  for (const auto& v : vocabulary) {
    int syllable_id = v.first;
    auto& node(index->at[count++]);
    node.key = syllable_id;
    const auto& entries(v.second.entries);
    if (!BuildEntryList(entries, &node.entries)) {
      return NULL;
    }
    if (v.second.next_level) {
      Code code(prefix);
      code.push_back(syllable_id);
      if (code.size() < Code::kIndexCodeMaxLength) {
        auto next_level_index = BuildTrunkIndex(code, *v.second.next_level);
        if (!next_level_index) {
          return NULL;
        }
        node.next_level =
            reinterpret_cast<table::PhraseIndex*>(next_level_index);
      } else {
        auto tail_index = BuildTailIndex(code, *v.second.next_level);
        if (!tail_index) {
          return NULL;
        }
        node.next_level = reinterpret_cast<table::PhraseIndex*>(tail_index);
      }
    }
  }
  return index;
}

table::TailIndex* Table::BuildTailIndex(const Code& prefix,
                                        const Vocabulary& vocabulary) {
  if (vocabulary.find(-1) == vocabulary.end()) {
    return NULL;
  }
  const auto& page(vocabulary.find(-1)->second);
  DLOG(INFO) << "page size: " << page.entries.size();
  auto index = CreateArray<table::LongEntry>(page.entries.size());
  if (!index) {
    return NULL;
  }
  size_t count = 0;
  for (const auto& src : page.entries) {
    DLOG(INFO) << "count: " << count;
    DLOG(INFO) << "entry: " << src->text;
    auto& dest(index->at[count++]);
    size_t extra_code_length = src->code.size() - Code::kIndexCodeMaxLength;
    DLOG(INFO) << "extra code length: " << extra_code_length;
    dest.extra_code.size = extra_code_length;
    dest.extra_code.at = Allocate<SyllableId>(extra_code_length);
    if (!dest.extra_code.at) {
      LOG(ERROR) << "Error creating code sequence; file size: " << file_size();
      return NULL;
    }
    std::copy(src->code.begin() + Code::kIndexCodeMaxLength, src->code.end(),
              dest.extra_code.begin());
    BuildEntry(*src, &dest.entry);
  }
  return index;
}

Array<table::Entry>* Table::BuildEntryArray(const ShortDictEntryList& entries) {
  auto array = CreateArray<table::Entry>(entries.size());
  if (!array) {
    return NULL;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!BuildEntry(*entries[i], &array->at[i])) {
      return NULL;
    }
  }
  return array;
}

bool Table::BuildEntryList(const ShortDictEntryList& src,
                           List<table::Entry>* dest) {
  if (!dest)
    return false;
  dest->size = src.size();
  dest->at = Allocate<table::Entry>(src.size());
  if (!dest->at) {
    LOG(ERROR) << "Error creating table entries; file size: " << file_size();
    return false;
  }
  size_t i = 0;
  for (auto d = src.begin(); d != src.end(); ++d, ++i) {
    if (!BuildEntry(**d, &dest->at[i]))
      return false;
  }
  return true;
}

bool Table::BuildEntry(const ShortDictEntry& dict_entry, table::Entry* entry) {
  if (!entry)
    return false;
  if (!AddString(dict_entry.text, &entry->text, dict_entry.weight)) {
    LOG(ERROR) << "Error creating table entry '" << dict_entry.text
               << "'; file size: " << file_size();
    return false;
  }
  entry->weight = static_cast<table::Weight>(dict_entry.weight);
  return true;
}

bool Table::GetSyllabary(Syllabary* result) {
  if (!result || !syllabary_)
    return false;
  for (size_t i = 0; i < syllabary_->size; ++i) {
    result->insert(GetSyllableById(static_cast<SyllableId>(i)));
  }
  return true;
}
string Table::GetSyllableById(SyllableId syllable_id) {
  if (!syllabary_ || syllable_id < 0 ||
      syllable_id >= static_cast<SyllableId>(syllabary_->size))
    return string();
  return GetString(syllabary_->at[syllable_id]);
}

TableAccessor Table::QueryWords(SyllableId syllable_id) {
  TableQuery query(index_);
  return query.Access(syllable_id);
}

TableAccessor Table::QueryPhrases(const Code& code) {
  if (code.empty())
    return TableAccessor();
  TableQuery query(index_);
  for (size_t i = 0; i < Code::kIndexCodeMaxLength; ++i) {
    if (code.size() == i + 1)
      return query.Access(code[i]);
    if (!query.Advance(code[i]))
      return TableAccessor();
  }
  return query.Access(-1);
}

// log(0.05) ≈ -3.0
const double kPenaltyForAmbiguousSyllable = -2.995732274;

// 蓬萊：Table::Query 的 BFS 狀態。跟 TableQuery 一樣走三層索引、累加 credibility 和 quality_len，
// 只是碼放固定長度的陣列、不帶 vector，進佇列不配置記憶體（原本每推一個狀態要複製四個 vector）
namespace {

struct QueryState {
  size_t pos;
  size_t level;
  SyllableId code[Code::kIndexCodeMaxLength];
  double credibility;  // TableQuery::credibility_sum()
  double quality_len;  // TableQuery::quality_len_sum()
  table::TrunkIndex* lv2;
  table::TrunkIndex* lv3;
  table::TailIndex* lv4;

  // 碼放進 out（最多 kIndexCodeMaxLength 個），回傳長度：前 level 個，last ≥ 0 時再接上它
  size_t MakeCode(SyllableId last, SyllableId* out) const {
    std::copy(code, code + level, out);
    if (last < 0)
      return level;
    out[level] = last;
    return level + 1;
  }
};

}  // namespace

bool Table::Query(const SyllableGraph& syll_graph,
                  size_t start_pos,
                  TableQueryResult* result,
                  size_t* watermark) {
  if (!result || !index_ || start_pos >= syll_graph.interpreted_length)
    return false;
  result->clear();
  // 蓬萊：結果跟原本照 TableQuery 一步一步走的相同（同樣的順序推進佇列、同樣的順序收結果）。
  // 每個音節只找一次節點，Access 和 Walk 共用；沒有詞的節點不建 TableAccessor
  // 佇列每次清空重用（同一個執行緒不會同時跑兩個 Query），不用每次重新配置、長大
  thread_local vector<QueryState> q;
  q.clear();
  q.push_back({start_pos, 0, {}, 0, 0, nullptr, nullptr, nullptr});
  for (size_t head = 0; head < q.size(); ++head) {
    const QueryState query = q[head];
    size_t current_pos = query.pos;
    if (watermark && current_pos > *watermark)
      *watermark = current_pos;
    auto index = syll_graph.indices.find(current_pos);
    if (index == syll_graph.indices.end()) {
      continue;
    }
    if (query.level == Code::kIndexCodeMaxLength) {
      // TableQuery::Access(-1)
      if (query.lv4 && query.lv4->size) {
        SyllableId code[Code::kIndexCodeMaxLength];
        const size_t n = query.MakeCode(-1, code);
        (*result)[current_pos].push_back(
            TableAccessor(code, n, query.lv4, 0 + query.credibility,
                          0 + query.quality_len));
      }
      continue;
    }
    // 找到節點的音節：收詞、往下一層（TableQuery::Access、Walk 找的是同一個節點）
    auto visit = [&](SyllableId syll_id,
                     const SpellingPropertiesList& spellings,
                     const List<table::Entry>* entries,
                     const OffsetPtr<table::PhraseIndex>* next_level) {
      const bool has_entries = entries->size && entries->at;
      const bool can_advance = *next_level;
      for (auto props : spellings) {
        size_t end_pos = props->end_pos;
        if (watermark && end_pos > *watermark)
          *watermark = end_pos;

        double penalty = 0.0;
        double next_credibility = props->credibility + penalty;

        // 全碼匹配長度積分
        bool is_normal_spelling = props->type == kNormalSpelling;
        double delta_quality_len =
            (is_normal_spelling ? 1.0 : 0.0) * (end_pos - current_pos);
        if (has_entries) {
          SyllableId code[Code::kIndexCodeMaxLength];
          const size_t n = query.MakeCode(syll_id, code);
          (*result)[end_pos].push_back(TableAccessor(
              code, n, entries, next_credibility + query.credibility,
              delta_quality_len + query.quality_len));
        }
        if (end_pos < syll_graph.interpreted_length && can_advance) {
          QueryState next = query;
          next.pos = end_pos;
          next.code[query.level] = syll_id;
          next.level = query.level + 1;
          next.credibility = query.credibility + next_credibility;
          next.quality_len = query.quality_len + delta_quality_len;
          if (query.level == 0)
            next.lv2 = &(*next_level)->trunk();
          else if (query.level == 1)
            next.lv3 = &(*next_level)->trunk();
          else
            next.lv4 = &(*next_level)->tail();
          q.push_back(next);
        }
      }
    };
    const auto& spelling_index = index->second;
    if (query.level == 0) {
      for (const auto& spellings : spelling_index) {
        SyllableId syll_id = spellings.first;
        if (syll_id >= 0 && syll_id < static_cast<SyllableId>(index_->size)) {
          auto node = &index_->at[syll_id];
          visit(syll_id, spellings.second, &node->entries, &node->next_level);
        }
      }
      continue;
    }
    auto trunk = query.level == 1 ? query.lv2 : query.lv3;
    if (!trunk)
      continue;  // Walk 失敗：每個音節都找不到節點
    // 第二、三層：音節（spelling_index）和這一層的節點（trunk）都照音節編號由小到大排，原本每個音節各自
    // 在節點裡二分搜尋（find_node）。改成照順序往後找、節點找完就停；節點比音節少很多時反過來，逐一找音節。
    // 處理的音節、順序都跟原本相同（節點的編號不重複時；重複時原本只會找到第一個，這裡也只用第一個）
    if (spelling_index.empty())
      continue;
    // 只看編號落在這些音節最小到最大之間的節點（簡拼的音節是連續的一段）
    table::TrunkIndexNode target;
    target.key = spelling_index.begin()->first;
    table::TrunkIndexNode* first =
        std::lower_bound(trunk->begin(), trunk->end(), target, node_less);
    target.key = spelling_index.rbegin()->first;
    table::TrunkIndexNode* last =
        std::upper_bound(first, trunk->end(), target, node_less);
    if (first == last)
      continue;
    if (static_cast<size_t>(last - first) * 4 < spelling_index.size()) {
      for (auto node = first; node != last; ++node) {
        if (node != trunk->begin() && (node - 1)->key == node->key)
          continue;
        auto found = spelling_index.find(node->key);
        if (found != spelling_index.end())
          visit(node->key, found->second, &node->entries, &node->next_level);
      }
    } else {
      auto node = first;
      for (const auto& spellings : spelling_index) {
        SyllableId syll_id = spellings.first;
        // 從上一個位置往後指數搜尋
        if (node->key < syll_id) {
          size_t step = 1;
          auto lo = node + 1;
          while (lo + step < last && (lo + step - 1)->key < syll_id) {
            lo += step;
            step <<= 1;
          }
          target.key = syll_id;
          node = std::lower_bound(lo, (std::min)(lo + step, last), target,
                                  node_less);
          if (node == last)
            break;
        }
        if (syll_id < node->key)
          continue;
        visit(syll_id, spellings.second, &node->entries, &node->next_level);
      }
    }
  }
  return !result->empty();
}

string Table::GetEntryText(const table::Entry& entry) {
  return GetString(entry.text);
}

}  // namespace rime
