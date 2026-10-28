//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 蓬萊：英文單字表（translator/english_lexicon）
//
#include <fstream>
#include <mutex>
#include <rime/deployer.h>
#include <rime/service.h>
#include <rime/gear/english_lexicon.h>

namespace rime {

static bool is_lowercase_word(const string& word) {
  if (word.empty())
    return false;
  for (char c : word) {
    if (c < 'a' || c > 'z')
      return false;
  }
  return true;
}

an<EnglishLexicon> EnglishLexicon::Require(const string& name,
                                           int min_length,
                                           double min_zipf) {
  if (name.empty())
    return nullptr;
  const auto& deployer = Service::instance().deployer();
  path file;
  for (const auto& dir : {deployer.user_data_dir, deployer.shared_data_dir}) {
    path candidate = dir / (name + ".txt");
    if (std::filesystem::exists(candidate)) {
      file = candidate;
      break;
    }
  }
  if (file.empty()) {
    LOG(WARNING) << "english lexicon not found: " << name << ".txt";
    return nullptr;
  }
  std::error_code ec;
  auto mtime = std::filesystem::last_write_time(file, ec);
  string key = file.u8string() + "\t" + std::to_string(min_length) + "\t" +
               std::to_string(min_zipf) + "\t" +
               std::to_string(static_cast<long long>(
                   mtime.time_since_epoch().count()));
  static std::mutex mutex;
  static map<string, an<EnglishLexicon>> cache;
  std::lock_guard<std::mutex> lock(mutex);
  auto found = cache.find(key);
  if (found != cache.end())
    return found->second;

  auto lexicon = New<EnglishLexicon>();
  std::ifstream in(file.c_str());
  string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    size_t tab = line.find('\t');
    if (tab == string::npos)
      continue;
    string word = line.substr(0, tab);
    if (!is_lowercase_word(word) ||
        static_cast<int>(word.length()) < min_length)
      continue;
    double zipf = std::atof(line.c_str() + tab + 1);
    if (zipf < min_zipf)
      continue;
    lexicon->words_[word] = zipf;
    lexicon->max_length_ = (std::max)(lexicon->max_length_, word.length());
  }
  LOG(INFO) << "english lexicon: " << lexicon->size() << " words from "
            << file << " (min_length " << min_length << ", min_zipf "
            << min_zipf << ")";
  if (lexicon->words_.empty())
    lexicon.reset();
  cache[key] = lexicon;
  return lexicon;
}

an<EnglishJudge> EnglishLexicon::RequireJudge(const string& name) {
  if (name.empty())
    return nullptr;
  const auto& deployer = Service::instance().deployer();
  path file;
  for (const auto& dir : {deployer.user_data_dir, deployer.shared_data_dir}) {
    path candidate = dir / (name + ".tbl");
    if (std::filesystem::exists(candidate)) {
      file = candidate;
      break;
    }
  }
  if (file.empty()) {
    LOG(WARNING) << "english judge table not found: " << name << ".tbl";
    return nullptr;
  }
  std::error_code ec;
  auto mtime = static_cast<long long>(
      std::filesystem::last_write_time(file, ec).time_since_epoch().count());
  // 只留弱參照：沒有翻譯器在用（重新部署之後）就放掉 mmap；檔案換過（修改時間不同）就重開
  static std::mutex mutex;
  static map<string, pair<long long, weak<EnglishJudge>>> cache;
  std::lock_guard<std::mutex> lock(mutex);
  auto& slot = cache[file.u8string()];
  if (auto judge = slot.second.lock()) {
    if (slot.first == mtime)
      return judge;
  }
  string error;
  auto judge = EnglishJudge::Open(file.string(), &error);
  if (judge) {
    LOG(INFO) << "english judge: " << file << " (" << judge->file_size()
              << " bytes)";
  } else {
    LOG(ERROR) << "english judge: cannot load " << file << ": " << error;
  }
  slot = {mtime, judge};
  return judge;
}

bool EnglishLexicon::Find(const string& word, double* zipf) const {
  auto found = words_.find(word);
  if (found == words_.end())
    return false;
  if (zipf)
    *zipf = found->second;
  return true;
}

}  // namespace rime
