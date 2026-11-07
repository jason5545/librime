//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-11-02 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <rime/service.h>
#include <rime/algo/dynamics.h>
#include <rime/dict/text_db.h>
#include <rime/dict/user_db.h>

namespace rime {

UserDbValue::UserDbValue(const string& value) {
  Unpack(value);
}

string UserDbValue::Pack() const {
  std::ostringstream packed;
  packed << "c=" << commits << " d=" << dee << " t=" << tick;
  return packed.str();
}

bool UserDbValue::Unpack(const string& value) {
  // 蓬萊：同 boost::split(value, is_any_of(" ")) 逐段處理，不另外配置 vector 和 key 字串
  //（學習紀錄每查到一筆就解一次，原本佔查學習紀錄的一成多）
  for (size_t begin = 0; begin <= value.length();) {
    size_t end = value.find(' ', begin);
    if (end == string::npos)
      end = value.length();
    size_t eq = value.find('=', begin);
    if (eq != string::npos && eq < end) {
      const bool one = eq - begin == 1;
      const char k = value[begin];
      string v(value, eq + 1, end - eq - 1);
      try {
        if (one && k == 'c') {
          commits = std::stoi(v);
        } else if (one && k == 'd') {
          dee = (std::min)(10000.0, std::stod(v));
        } else if (one && k == 't') {
          tick = std::stoul(v);
        }
      } catch (...) {
        LOG(ERROR) << "failed in parsing key-value from userdb entry '"
                   << value.substr(begin, end - begin) << "'.";
        return false;
      }
    }
    begin = end + 1;
  }
  return true;
}

static const string plain_userdb_extension(".userdb.txt");

template <>
string UserDbComponent<TextDb>::extension() const {
  return plain_userdb_extension;
}

string UserDb::snapshot_extension() {
  return plain_userdb_extension;
}

// key ::= code <space> <Tab> phrase

static bool userdb_entry_parser(const Tsv& row, string* key, string* value) {
  if (row.size() < 2 || row[0].empty() || row[1].empty()) {
    return false;
  }
  string code(row[0]);
  // fix invalid keys created by a buggy version
  if (code[code.length() - 1] != ' ')
    code += ' ';
  *key = code + "\t" + row[1];
  if (row.size() >= 3)
    *value = row[2];
  else
    value->clear();
  return true;
}

static bool userdb_entry_formatter(const string& key,
                                   const string& value,
                                   Tsv* tsv) {
  Tsv& row(*tsv);
  boost::algorithm::split(row, key, boost::algorithm::is_any_of("\t"));
  if (row.size() != 2 || row[0].empty() || row[1].empty())
    return false;
  row.push_back(value);
  return true;
}

static TextFormat plain_userdb_format = {
    userdb_entry_parser,
    userdb_entry_formatter,
    "Rime user dictionary",
};

template <>
RIME_DLL UserDbWrapper<TextDb>::UserDbWrapper(const path& file_path,
                                              const string& db_name)
    : TextDb(file_path, db_name, "userdb", plain_userdb_format) {}

bool UserDbHelper::UpdateUserInfo() {
  Deployer& deployer(Service::instance().deployer());
  return db_->MetaUpdate("/user_id", deployer.user_id);
}

bool UserDbHelper::IsUniformFormat(const path& file_path) {
  return boost::ends_with(file_path.filename().u8string(),
                          plain_userdb_extension);
}

bool UserDbHelper::UniformBackup(const path& snapshot_file) {
  LOG(INFO) << "backing up userdb '" << db_->name() << "' to " << snapshot_file;
  TsvWriter writer(snapshot_file, plain_userdb_format.formatter);
  writer.file_description = plain_userdb_format.file_description;
  DbSource source(db_);
  try {
    writer << source;
  } catch (std::exception& ex) {
    LOG(ERROR) << ex.what();
    return false;
  }
  return true;
}

bool UserDbHelper::UniformRestore(const path& snapshot_file) {
  LOG(INFO) << "restoring userdb '" << db_->name() << "' from "
            << snapshot_file;
  TsvReader reader(snapshot_file, plain_userdb_format.parser);
  DbSink sink(db_);
  try {
    reader >> sink;
  } catch (std::exception& ex) {
    LOG(ERROR) << ex.what();
    return false;
  }
  return true;
}

bool UserDbHelper::IsUserDb() {
  string db_type;
  return db_->MetaFetch("/db_type", &db_type) && (db_type == "userdb");
}

string UserDbHelper::GetDbName() {
  string name;
  if (!db_->MetaFetch("/db_name", &name))
    return name;
  auto ext = boost::find_last(name, ".userdb");
  if (!ext.empty()) {
    // remove ".userdb.*"
    name.erase(ext.begin(), name.end());
  }
  return name;
}

string UserDbHelper::GetUserId() {
  string user_id("unknown");
  db_->MetaFetch("/user_id", &user_id);
  return user_id;
}

string UserDbHelper::GetRimeVersion() {
  string version;
  db_->MetaFetch("/rime_version", &version);
  return version;
}

static TickCount get_tick_count(Db* db) {
  string tick;
  if (db && db->MetaFetch("/tick", &tick)) {
    try {
      return std::stoul(tick);
    } catch (...) {
    }
  }
  return 1;
}

UserDbMerger::UserDbMerger(Db* db) : db_(db) {
  our_tick_ = get_tick_count(db);
  their_tick_ = 0;
  max_tick_ = our_tick_;
}

UserDbMerger::~UserDbMerger() {
  CloseMerge();
}

bool UserDbMerger::MetaPut(const string& key, const string& value) {
  if (key == "/tick") {
    try {
      their_tick_ = std::stoul(value);
      max_tick_ = (std::max)(our_tick_, their_tick_);
    } catch (...) {
    }
  }
  return true;
}

// 蓬萊：一邊刪掉（commits 是負的）、一邊還在用（正的）時，留著還是刪掉照時間判斷，不再只比絕對值
// （原本本機刪掉的 −5 遇到對方的 9 就變回 9、刪掉的詞回來；對方很久以前刪掉的 −9 會蓋掉本機最近打過的 5）。
// 時間比的是兩邊詞條的 tick 本身：同步合併後學習紀錄的 tick 取兩邊大的（CloseMerge），之後各自往上加，
// 跟 Lamport 時鐘一樣，同步過的兩台、還有自己上一次的快照（每次同步都會合併回來），tick 大的就是後發生的。
// 不換成「距離各自的 tick 多遠」：快照的 tick 是寫快照那時候的，拿它當現在，同步之後才刪的詞會被自己的舊快照救回來。
// 一樣大時刪掉的贏：刪除不會讓 tick 加一，同一台上刪除跟它前一次打字的 tick 相同、發生在後面。
// 絕對值（打過幾次）照舊取大的，只決定正負號；兩邊同號、有一邊是 0 或本機沒有這筆時跟原本一樣
bool UserDbMerger::Put(const string& key, const string& value) {
  if (!db_)
    return false;
  UserDbValue v(value);
  const TickCount v_tick =
      (their_tick_ && v.tick > their_tick_) ? their_tick_ : v.tick;
  if (v.tick < their_tick_) {
    v.dee = algo::formula_d(0, (double)their_tick_, v.dee, (double)v.tick);
  }
  UserDbValue o;
  string our_value;
  if (db_->Fetch(key, &our_value)) {
    o.Unpack(our_value);
  }
  const TickCount o_tick = o.tick > our_tick_ ? our_tick_ : o.tick;
  if (o.tick < our_tick_) {
    o.dee = algo::formula_d(0, (double)our_tick_, o.dee, (double)o.tick);
  }
  if (their_tick_ && o.commits != 0 && v.commits != 0 &&
      (o.commits < 0) != (v.commits < 0)) {
    const int magnitude = (std::max)(std::abs(o.commits), std::abs(v.commits));
    const bool deleted = v_tick > o_tick   ? v.commits < 0
                         : o_tick > v_tick ? o.commits < 0
                                           : true;
    o.commits = deleted ? -magnitude : magnitude;
  } else if (std::abs(o.commits) < std::abs(v.commits)) {
    o.commits = v.commits;
  }
  o.dee = (std::max)(o.dee, v.dee);
  o.tick = max_tick_;
  return db_->Update(key, o.Pack()) && ++merged_entries_;
}

void UserDbMerger::CloseMerge() {
  if (!db_ || !merged_entries_)
    return;
  Deployer& deployer(Service::instance().deployer());
  try {
    db_->MetaUpdate("/tick", std::to_string(max_tick_));
    db_->MetaUpdate("/user_id", deployer.user_id);
  } catch (...) {
    LOG(ERROR) << "failed to update tick count.";
    return;
  }
  LOG(INFO) << "total " << merged_entries_
            << " entries merged, tick = " << max_tick_;
  merged_entries_ = 0;
}

UserDbImporter::UserDbImporter(Db* db) : db_(db) {}

bool UserDbImporter::MetaPut(const string& key, const string& value) {
  return true;
}

bool UserDbImporter::Put(const string& key, const string& value) {
  if (!db_)
    return false;
  UserDbValue v(value);
  UserDbValue o;
  string old_value;
  if (db_->Fetch(key, &old_value)) {
    o.Unpack(old_value);
  }
  if (v.commits > 0) {
    o.commits = (std::max)(o.commits, v.commits);
    o.dee = (std::max)(o.dee, v.dee);
  } else if (v.commits < 0) {  // mark as deleted
    o.commits = (std::min)(v.commits, -std::abs(o.commits));
  }
  return db_->Update(key, o.Pack());
}

}  // namespace rime
