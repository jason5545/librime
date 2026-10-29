//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2012-04-22 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <ctime>
#include <fstream>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/config.h>
#include <rime/deployer.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/gear/grammar.h>
#include <rime/gear/translator_commons.h>

namespace rime {

// Patterns

bool Patterns::Load(an<ConfigList> patterns) {
  clear();
  if (!patterns)
    return false;
  for (auto it = patterns->begin(); it != patterns->end(); ++it) {
    if (auto value = As<ConfigValue>(*it)) {
      push_back(boost::regex(value->str()));
    }
  }
  return true;
}

// Spans

void Spans::AddVertex(size_t vertex) {
  if (vertices_.empty() || vertices_.back() < vertex) {
    vertices_.push_back(vertex);
    return;
  }
  auto lb = std::lower_bound(vertices_.begin(), vertices_.end(), vertex);
  if (*lb != vertex) {
    vertices_.insert(lb, vertex);
  }
}

void Spans::AddSpan(size_t start, size_t end) {
  AddVertex(start);
  AddVertex(end);
}

void Spans::AddSpans(const Spans& spans) {
  for (auto vertex : spans.vertices_) {
    AddVertex(vertex);
  }
}

void Spans::Clear() {
  vertices_.clear();
}

size_t Spans::PreviousStop(size_t caret_pos) const {
  for (auto x : boost::adaptors::reverse(vertices_)) {
    if (x < caret_pos)
      return x;
  }
  return caret_pos;
}

size_t Spans::NextStop(size_t caret_pos) const {
  for (auto x : vertices_) {
    if (x > caret_pos)
      return x;
  }
  return caret_pos;
}

size_t Spans::Count(size_t start_pos, size_t end_pos) const {
  size_t count = 0;
  for (auto v : vertices_) {
    if (v <= start_pos)
      continue;
    else if (v > end_pos)
      break;
    else
      ++count;
  }
  return count;
}

bool Spans::HasVertex(size_t vertex) const {
  return std::binary_search(vertices_.begin(), vertices_.end(), vertex);
}

// Sentence

void Sentence::Extend(const DictEntry& another,
                      size_t end_pos,
                      double new_weight) {
  entry_->weight = new_weight;
  entry_->text.append(another.text);
  entry_->code.insert(entry_->code.end(), another.code.begin(),
                      another.code.end());
  components_.push_back(another);
  word_lengths_.push_back(end_pos - end());
  set_end(end_pos);
  DLOG(INFO) << "extend sentence " << end_pos << ") " << text()
             << " weight: " << weight();
}

void Sentence::Offset(size_t offset) {
  set_start(start() + offset);
  set_end(end() + offset);
}

// TranslatorOptions

TranslatorOptions::TranslatorOptions(const Ticket& ticket) {
  if (!ticket.schema)
    return;
  if (Config* config = ticket.schema->config()) {
    config->GetString(ticket.name_space + "/delimiter", &delimiters_) ||
        config->GetString("speller/delimiter", &delimiters_);
    config->GetBool(ticket.name_space + "/contextual_suggestions",
                    &contextual_suggestions_);
    config->GetBool(ticket.name_space + "/last_commit_as_context",
                    &last_commit_as_context_);
    config->GetBool(ticket.name_space + "/enable_completion",
                    &enable_completion_);
    config->GetBool(ticket.name_space + "/strict_spelling", &strict_spelling_);
    config->GetDouble(ticket.name_space + "/initial_quality",
                      &initial_quality_);
    config->GetInt(ticket.name_space + "/max_sentences", &max_sentences_);
    max_sentences_ = std::min(std::max(1, max_sentences_), 100);
    config->GetDouble(ticket.name_space + "/sentence_cutoff_threshold",
                      &sentence_cutoff_threshold_);

    preedit_formatter_.Load(
        config->GetList(ticket.name_space + "/preedit_format"));
    comment_formatter_.Load(
        config->GetList(ticket.name_space + "/comment_format"));
    user_dict_disabling_patterns_.Load(
        config->GetList(ticket.name_space + "/disable_user_dict_for_patterns"));

    string tag;
    if (config->GetString(ticket.name_space + "/tag", &tag)) {
      // replace the first tag, and understand /tags as extra tags
      tags_[0] = tag;
    } else {
      // replace all of the default tags
      tags_.clear();
    }
    if (auto list = config->GetList(ticket.name_space + "/tags"))
      for (size_t i = 0; i < list->size(); ++i)
        if (auto value = As<ConfigValue>(list->GetAt(i)))
          tags_.push_back(value->str());
    if (tags_.empty())
      tags_.push_back("abc");

    // blacklist
    if (auto blacklist =
            config->GetList(ticket.name_space + "/dictionary_exclude")) {
      for (auto it = blacklist->begin(); it != blacklist->end(); ++it) {
        if (auto blackword = As<ConfigValue>(*it)) {
          blacklist_.insert(blackword->str());
        }
      }
    }
    // 蓬萊：隱藏詞清單（使用者資料夾裡的檔案）
    string exclude_file;
    if (config->GetString(ticket.name_space + "/dictionary_exclude_file",
                          &exclude_file) &&
        !exclude_file.empty()) {
      config->GetInt(ticket.name_space + "/dictionary_exclude_min_length",
                     &dictionary_exclude_min_length_);
      config_blacklist_ = blacklist_;
      excluded_words_ = ExcludedWords::Require(
          Service::instance().deployer().user_data_dir / exclude_file);
      RefreshBlacklist();
    }
  }
  if (delimiters_.empty()) {
    delimiters_ = " ";
  }
}

void TranslatorOptions::RefreshBlacklist() {
  if (!excluded_words_)
    return;
  excluded_words_->Refresh();
  if (excluded_words_->version() == excluded_version_)
    return;
  blacklist_ = config_blacklist_;
  blacklist_.insert(excluded_words_->words().begin(),
                    excluded_words_->words().end());
  excluded_version_ = excluded_words_->version();
}

// ExcludedWords

an<ExcludedWords> ExcludedWords::Require(const path& file) {
  static map<string, std::weak_ptr<ExcludedWords>> registry;
  const string key = file.string();
  if (auto found = registry[key].lock())
    return found;
  auto created = New<ExcludedWords>(file);
  registry[key] = created;
  return created;
}

void ExcludedWords::Refresh() {
  std::error_code ec;
  const bool exists = std::filesystem::is_regular_file(file_, ec);
  uintmax_t size = 0;
  std::filesystem::file_time_type mtime;
  if (exists) {
    size = std::filesystem::file_size(file_, ec);
    mtime = std::filesystem::last_write_time(file_, ec);
  }
  if (checked_ && exists == exists_ && size == size_ && mtime == mtime_)
    return;
  checked_ = true;
  exists_ = exists;
  size_ = size;
  mtime_ = mtime;
  Load();
}

void ExcludedWords::Load() {
  hash_set<string> words;
  std::ifstream in(file_.string());
  string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#' || line[0] == '\t')
      continue;
    words.insert(line.substr(0, line.find('\t')));
  }
  if (words != words_) {
    words_.swap(words);
    ++version_;
    LOG(INFO) << "dictionary_exclude_file: " << words_.size()
              << " words from " << file_;
  }
}

bool ExcludedWords::Add(const string& word) {
  if (word.empty() || word.find_first_of("\t\n") != string::npos)
    return false;
  if (Has(word))
    return true;
  std::ofstream out(file_.string(), std::ios::app);
  if (!out) {
    LOG(ERROR) << "dictionary_exclude_file: cannot write " << file_;
    return false;
  }
  char stamp[32];
  std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S",
                std::localtime(&now));
  out << word << '\t' << stamp << '\n';
  out.close();
  if (!out) {
    LOG(ERROR) << "dictionary_exclude_file: cannot write " << file_;
    return false;
  }
  words_.insert(word);
  ++version_;
  checked_ = false;  // 下次 Refresh 記下新的大小、時間（內容相同，不會再加版本）
  return true;
}

bool TranslatorOptions::IsUserDictDisabledFor(const string& input) const {
  if (user_dict_disabling_patterns_.empty())
    return false;
  for (const auto& pattern : user_dict_disabling_patterns_) {
    if (boost::regex_match(input, pattern))
      return true;
  }
  return false;
}

}  // namespace rime
