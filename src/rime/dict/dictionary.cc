//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-05 GONG Chen <chen.sst@gmail.com>
//
#include <filesystem>
#include <rime/algo/syllabifier.h>
#include <rime/common.h>
#include <rime/dict/dictionary.h>
#include <rime/resource.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <utility>

namespace rime {

namespace dictionary {

struct Chunk {
  Table* table = nullptr;
  SmallCode code;  // 蓬萊：碼放在物件裡（table.h），索引碼不另外配置
  const table::Entry* entries = nullptr;
  size_t size = 0;
  size_t cursor = 0;
  string remaining_code;  // for predictive queries
  size_t matching_code_size = 0;
  double credibility = 0.0;
  double quality_len = 0.0;

  Chunk() = default;
  Chunk(Table* t,
        const Code& c,
        const table::Entry* e,
        size_t m,
        double cr = 0.0,
        double q = 0.0)
      : table(t),
        code(c),
        entries(e),
        size(1),
        cursor(0),
        matching_code_size(m),
        credibility(cr),
        quality_len(q) {}
  Chunk(Table* t, const TableAccessor& a, double cr = 0.0, double q = 0.0)
      : Chunk(t, a, string(), cr, q) {}
  Chunk(Table* t,
        const TableAccessor& a,
        const string& r,
        double cr = 0.0,
        double q = 0.0)
      : table(t),
        code(a.small_index_code()),
        entries(a.entry()),
        size(a.remaining()),
        cursor(0),
        remaining_code(r),
        matching_code_size(a.index_code_size()),
        credibility(cr),
        quality_len(q) {}

  bool is_exact_match() const { return matching_code_size == code.size(); }

  bool is_predictive_match() const { return matching_code_size < code.size(); }
};

struct QueryResult {
  vector<Chunk> chunks;
};

bool compare_chunk_by_head_element(const Chunk& a, const Chunk& b) {
  if (!a.entries || a.cursor >= a.size)
    return false;
  if (!b.entries || b.cursor >= b.size)
    return true;
  if (a.is_exact_match() != b.is_exact_match())
    return a.is_exact_match() > b.is_exact_match();
  if (a.remaining_code.length() != b.remaining_code.length())
    return a.remaining_code.length() < b.remaining_code.length();
  return a.credibility + a.entries[a.cursor].weight >
         b.credibility + b.entries[b.cursor].weight;  // by weight desc
}

struct CodeMatch {
  bool success;
  size_t depth;
  size_t end_pos;
};

CodeMatch match_extra_code(const table::Code* extra_code,
                           size_t depth,
                           const SyllableGraph& syll_graph,
                           size_t current_pos,
                           bool predict_word,
                           size_t* watermark = nullptr) {
  const CodeMatch kFailed{false, 0, 0};
  if (watermark && current_pos > *watermark)
    *watermark = current_pos;
  if (!extra_code || depth >= extra_code->size)
    return {true, depth, current_pos};
  if (current_pos >= syll_graph.interpreted_length) {
    if (predict_word)
      return {true, depth, syll_graph.interpreted_length};
    else
      return kFailed;
  }
  auto index = syll_graph.indices.find(current_pos);
  if (index == syll_graph.indices.end())
    return kFailed;
  SyllableId current_syll_id = extra_code->at[depth];
  auto spellings = index->second.find(current_syll_id);
  if (spellings == index->second.end())
    return kFailed;
  CodeMatch best_match = kFailed;
  for (const SpellingProperties* props : spellings->second) {
    CodeMatch match = match_extra_code(extra_code, depth + 1, syll_graph,
                                       props->end_pos, predict_word, watermark);
    if (!match.success)
      continue;
    if (match.end_pos > best_match.end_pos)
      best_match = match;
  }
  return best_match;
}

}  // namespace dictionary

DictEntryIterator::DictEntryIterator()
    : query_result_(New<dictionary::QueryResult>()) {}

void DictEntryIterator::AddChunk(dictionary::Chunk&& chunk) {
  query_result_->chunks.push_back(std::move(chunk));
  entry_count_ += chunk.size;
}

void DictEntryIterator::Sort() {
  auto& chunks = query_result_->chunks;
  // partial-sort remaining chunks, move best match to chunk_index_
  std::partial_sort(chunks.begin() + chunk_index_,
                    chunks.begin() + chunk_index_ + 1, chunks.end(),
                    dictionary::compare_chunk_by_head_element);
}

void DictEntryIterator::AddFilter(DictEntryFilter filter) {
  DictEntryFilterBinder::AddFilter(filter);
  // the introduced filter could invalidate the current or even all the
  // remaining entries
  while (!exhausted() && !filter_(Peek())) {
    entry_.reset();
    FindNextEntry();
  }
}

an<DictEntry> DictEntryIterator::Peek() {
  if (!entry_ && !exhausted()) {
    // get next entry from current chunk
    const auto& chunk = query_result_->chunks[chunk_index_];
    const auto& e = chunk.entries[chunk.cursor];
    DLOG(INFO) << "creating temporary dict entry '"
               << chunk.table->GetEntryText(e) << "'.";
    entry_ = New<DictEntry>();
    entry_->code.assign(chunk.code.data(),
                        chunk.code.data() + chunk.code.size());
    entry_->text = chunk.table->GetEntryText(e);
    const double kS = 18.420680743952367;  // log(1e8)
    entry_->weight = e.weight - kS + chunk.credibility;
    entry_->quality_len = chunk.quality_len;
    if (!chunk.remaining_code.empty()) {
      entry_->comment = "~" + chunk.remaining_code;
      entry_->remaining_code_length = chunk.remaining_code.length();
    }
    if (chunk.is_predictive_match()) {
      entry_->matching_code_size = chunk.matching_code_size;
    }
  }
  return entry_;
}

bool DictEntryIterator::FindNextEntry() {
  if (exhausted()) {
    return false;
  }
  auto& chunk = query_result_->chunks[chunk_index_];
  if (++chunk.cursor >= chunk.size) {
    ++chunk_index_;
  }
  if (exhausted()) {
    return false;
  }
  // reorder chunks to move the one with the best entry to head
  Sort();
  return true;
}

bool DictEntryIterator::Next() {
  do {
    entry_.reset();
    if (!FindNextEntry()) {
      return false;
    }
  } while (filter_ && !filter_(Peek()));
  return true;
}

// Note: does not apply filters
bool DictEntryIterator::Skip(size_t num_entries) {
  while (num_entries > 0) {
    if (exhausted())
      return false;
    auto& chunk = query_result_->chunks[chunk_index_];
    if (chunk.cursor + num_entries < chunk.size) {
      chunk.cursor += num_entries;
      return true;
    }
    num_entries -= (chunk.size - chunk.cursor);
    ++chunk_index_;
  }
  return true;
}

bool DictEntryIterator::exhausted() const {
  return chunk_index_ >= query_result_->chunks.size();
}

bool DictEntryIterator::AnyRemainingCode(
    const std::function<bool(const Code&, bool)>& f) const {
  if (!query_result_)
    return false;
  for (size_t i = chunk_index_; i < query_result_->chunks.size(); ++i) {
    const auto& c = query_result_->chunks[i];
    if (!c.entries || c.cursor >= c.size)
      continue;
    Code code;
    code.assign(c.code.data(), c.code.data() + c.code.size());
    if (f(code, c.is_predictive_match()))
      return true;
  }
  return false;
}

// Dictionary members

Dictionary::Dictionary(string name,
                       vector<string> packs,
                       vector<of<Table>> tables,
                       an<Prism> prism)
    : name_(name),
      packs_(std::move(packs)),
      tables_(std::move(tables)),
      prism_(std::move(prism)) {}

Dictionary::~Dictionary() {
  // should not close shared table and prism objects
}

static void lookup_table(Table* table,
                         DictEntryCollector* collector,
                         const SyllableGraph& syllable_graph,
                         size_t start_pos,
                         bool predict_word,
                         double initial_credibility,
                         size_t* watermark) {
  TableQueryResult result;
  if (!table->Query(syllable_graph, start_pos, &result, watermark)) {
    return;
  }
  // copy result
  for (auto& v : result) {
    size_t end_pos = v.first;
    for (TableAccessor& a : v.second) {
      double cr = initial_credibility + a.credibility();
      double q = a.quality_len();
      if (a.extra_code()) {
        do {
          dictionary::CodeMatch match = dictionary::match_extra_code(
              a.extra_code(), 0, syllable_graph, end_pos, predict_word,
              watermark);
          if (!match.success)
            continue;
          size_t matching_code_size = a.index_code_size() + match.depth;
          (*collector)[match.end_pos].AddChunk(
              {table, a.code(), a.entry(), matching_code_size, cr, q});
        } while (a.Next());
      } else {
        (*collector)[end_pos].AddChunk({table, a, cr, q});
      }
    }
  }
}

an<DictEntryCollector> Dictionary::Lookup(const SyllableGraph& syllable_graph,
                                          size_t start_pos,
                                          const hash_set<string>* blacklist,
                                          bool predict_word,
                                          double initial_credibility,
                                          size_t* watermark) {
  if (!loaded())
    return nullptr;
  if (watermark && start_pos > *watermark)
    *watermark = start_pos;
  auto collector = New<DictEntryCollector>();
  for (const auto& table : tables_) {
    if (!table->IsOpen())
      continue;
    lookup_table(table.get(), collector.get(), syllable_graph, start_pos,
                 predict_word, initial_credibility, watermark);
  }
  if (collector->empty())
    return nullptr;
  // for each group of equal code length, sort it and filter words
  for (auto it = collector->begin(); it != collector->end();) {
    it->second.Sort();
    if (blacklist && !blacklist->empty()) {
      it->second.AddFilter([blacklist](an<DictEntry> entry) {
        return entry && !blacklist->count(entry->text);
      });
      // 蓬萊：整組都被擋掉時拿掉這組。留著的話 ScriptTranslation 會拿這個碼長跟學習紀錄比，
      // Peek() 是空的（擋掉「謝謝」再打 ㄧㄝˋ ㄒㄧㄝ˙ 時當掉）
      if (it->second.exhausted()) {
        it = collector->erase(it);
        continue;
      }
    }
    ++it;
  }
  if (collector->empty())
    return nullptr;
  return collector;
}

size_t Dictionary::LookupWords(DictEntryIterator* result,
                               const string& str_code,
                               bool predictive,
                               size_t expand_search_limit,
                               const hash_set<string>* blacklist) {
  DLOG(INFO) << "lookup: " << str_code;
  if (!loaded())
    return 0;
  vector<Prism::Match> keys;
  if (predictive) {
    prism_->ExpandSearch(str_code, &keys, expand_search_limit);
  } else {
    Prism::Match match{0, 0};
    if (prism_->GetValue(str_code, &match.value)) {
      keys.push_back(match);
    }
  }
  DLOG(INFO) << "found " << keys.size() << " matching keys thru the prism.";
  size_t code_length(str_code.length());
  for (auto& match : keys) {
    SpellingAccessor accessor(prism_->QuerySpelling(match.value));
    while (!accessor.exhausted()) {
      SyllableId syllable_id = accessor.syllable_id();
      SpellingType type = accessor.properties().type;
      accessor.Next();
      if (type > kNormalSpelling)
        continue;
      string remaining_code;
      if (match.length > code_length) {
        string syllable = primary_table()->GetSyllableById(syllable_id);
        if (syllable.length() > code_length)
          remaining_code = syllable.substr(code_length);
      }
      for (const auto& table : tables_) {
        if (!table->IsOpen())
          continue;
        TableAccessor a = table->QueryWords(syllable_id);
        if (!a.exhausted()) {
          DLOG(INFO) << "remaining code: " << remaining_code;
          result->AddChunk({table.get(), a, remaining_code});
        }
      }
    }
  }
  if (blacklist && !blacklist->empty()) {
    result->AddFilter([blacklist](an<DictEntry> entry) {
      return entry && !blacklist->count(entry->text);
    });
  }
  return keys.size();
}

bool Dictionary::Decode(const Code& code, vector<string>* result) {
  if (!result || tables_.empty())
    return false;
  result->clear();
  for (SyllableId c : code) {
    string s = primary_table()->GetSyllableById(c);
    if (s.empty())
      return false;
    result->push_back(s);
  }
  return true;
}

bool Dictionary::Exists() const {
  return std::filesystem::exists(prism_->file_path()) && !tables_.empty() &&
         std::filesystem::exists(tables_[0]->file_path());
}

bool Dictionary::Remove() {
  if (loaded())
    return false;
  prism_->Remove();
  for (const auto& table : tables_) {
    table->Remove();
  }
  return true;
}

bool Dictionary::Load() {
  LOG(INFO) << "loading dictionary '" << name_ << "'.";
  if (tables_.empty()) {
    LOG(ERROR) << "Cannot load dictionary '" << name_
               << "'; it contains no tables.";
    return false;
  }
  auto& primary_table = tables_[0];
  if (!primary_table || (!primary_table->IsOpen() && !primary_table->Load())) {
    LOG(ERROR) << "Error loading table for dictionary '" << name_ << "'.";
    return false;
  }
  if (!prism_ || (!prism_->IsOpen() && !prism_->Load())) {
    LOG(ERROR) << "Error loading prism for dictionary '" << name_ << "'.";
    return false;
  }
  // packs are optional
  for (int i = 1; i < tables_.size(); ++i) {
    const auto& table = tables_[i];
    if (!table->IsOpen() && table->Exists() && table->Load()) {
      LOG(INFO) << "loaded pack: " << packs_[i - 1];
    }
  }
  return true;
}

bool Dictionary::loaded() const {
  return !tables_.empty() && tables_[0]->IsOpen() && prism_ && prism_->IsOpen();
}

an<Table> Dictionary::FindVariant(const string& variant) {
  auto found = variant_tables_.find(variant);
  if (found != variant_tables_.end())
    return found->second;
  const auto& base = base_table_ ? base_table_ : tables_[0];
  an<Table> table;
  if (table_loader_ && base && base->IsOpen()) {
    table = table_loader_(name_ + "." + variant);
    if (table && !table->IsOpen() && !(table->Exists() && table->Load()))
      table.reset();
    if (table && table->dict_file_checksum() != base->dict_file_checksum()) {
      LOG(WARNING) << "variant table '" << table->file_path()
                   << "' does not match the primary table; ignored.";
      table.reset();
    }
    if (table)
      LOG(INFO) << "loaded variant table: " << table->file_path();
  }
  variant_tables_[variant] = table;
  return table;
}

bool Dictionary::HasVariant(const string& variant) {
  return !tables_.empty() && (variant.empty() || FindVariant(variant));
}

bool Dictionary::SelectVariant(const string& variant) {
  if (tables_.empty())
    return false;
  if (variant == variant_)
    return true;
  if (!base_table_)
    base_table_ = tables_[0];
  an<Table> table = variant.empty() ? nullptr : FindVariant(variant);
  tables_[0] = table ? table : base_table_;
  variant_ = table ? variant : string();
  return variant.empty() || table;
}

// DictionaryComponent members

static const ResourceType kPrismResourceType = {"prism", "", ".prism.bin"};

static const ResourceType kTableResourceType = {"table", "", ".table.bin"};

DictionaryComponent::DictionaryComponent()
    : prism_resource_resolver_(
          Service::instance().CreateDeployedResourceResolver(
              kPrismResourceType)),
      table_resource_resolver_(
          Service::instance().CreateDeployedResourceResolver(
              kTableResourceType)) {}

DictionaryComponent::~DictionaryComponent() {}

Dictionary* DictionaryComponent::Create(const Ticket& ticket) {
  if (!ticket.schema)
    return nullptr;
  Config* config = ticket.schema->config();
  string dict_name;
  if (!config->GetString(ticket.name_space + "/dictionary", &dict_name)) {
    LOG(ERROR) << ticket.name_space << "/dictionary not specified in schema '"
               << ticket.schema->schema_id() << "'.";
    return nullptr;
  }
  if (dict_name.empty()) {
    return nullptr;  // not requiring static dictionary
  }
  string prism_name;
  if (!config->GetString(ticket.name_space + "/prism", &prism_name)) {
    prism_name = dict_name;
  }
  vector<string> packs;
  if (auto pack_list = config->GetList(ticket.name_space + "/packs")) {
    for (const auto& item : *pack_list) {
      if (auto value = As<ConfigValue>(item)) {
        packs.push_back(value->str());
      }
    }
  }
  return Create(std::move(dict_name), std::move(prism_name), std::move(packs));
}

Dictionary* DictionaryComponent::Create(string dict_name,
                                        string prism_name,
                                        vector<string> packs) {
  // obtain prism and primary table objects
  auto primary_table = table_map_[dict_name].lock();
  if (!primary_table) {
    auto file_path = table_resource_resolver_->ResolvePath(dict_name);
    table_map_[dict_name] = primary_table = New<Table>(file_path);
  }
  auto prism = prism_map_[prism_name].lock();
  if (!prism) {
    auto file_path = prism_resource_resolver_->ResolvePath(prism_name);
    prism_map_[prism_name] = prism = New<Prism>(file_path);
  }
  vector<of<Table>> tables = {std::move(primary_table)};
  for (const auto& pack : packs) {
    auto table = table_map_[pack].lock();
    if (!table) {
      auto file_path = table_resource_resolver_->ResolvePath(pack);
      table_map_[pack] = table = New<Table>(file_path);
    }
    tables.push_back(std::move(table));
  }
  auto dictionary = new Dictionary(std::move(dict_name), std::move(packs),
                                   std::move(tables), std::move(prism));
  // 蓬萊：變體表跟主表一樣在所有 session 之間共用
  dictionary->set_table_loader([this](const string& table_name) {
    auto table = table_map_[table_name].lock();
    if (!table) {
      auto file_path = table_resource_resolver_->ResolvePath(table_name);
      table_map_[table_name] = table = New<Table>(file_path);
    }
    return table;
  });
  return dictionary;
}

}  // namespace rime
