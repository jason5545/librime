//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2014-12-04 Chen Gong <chen.sst@gmail.com>
//

#include <algorithm>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <rime/common.h>
#include <rime/service.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_db.h>

namespace rime {

static const char* kMetaCharacter = "\x01";

struct LevelDbCursor {
  leveldb::Iterator* iterator = nullptr;

  LevelDbCursor(leveldb::DB* db) {
    leveldb::ReadOptions options;
    options.fill_cache = false;
    iterator = db->NewIterator(options);
  }

  bool IsValid() const { return iterator && iterator->Valid(); }

  string GetKey() const { return iterator->key().ToString(); }

  string GetValue() const { return iterator->value().ToString(); }

  void Next() { iterator->Next(); }

  bool Jump(const string& key) {
    if (!iterator) {
      return false;
    }
    iterator->Seek(key);
    return true;
  }

  void Release() {
    delete iterator;
    iterator = nullptr;
  }
};

struct LevelDbWrapper {
  leveldb::DB* ptr = nullptr;
  leveldb::WriteBatch batch;

  leveldb::Status Open(const path& file_path, bool readonly) {
    leveldb::Options options;
    options.create_if_missing = !readonly;
    return leveldb::DB::Open(options, file_path.string(), &ptr);
  }

  void Release() {
    delete ptr;
    ptr = nullptr;
  }

  LevelDbCursor* CreateCursor() { return new LevelDbCursor(ptr); }

  bool Fetch(const string& key, string* value) {
    auto status = ptr->Get(leveldb::ReadOptions(), key, value);
    return status.ok();
  }

  bool Update(const string& key, const string& value, bool write_batch) {
    if (write_batch) {
      batch.Put(key, value);
      return true;
    }
    auto status = ptr->Put(leveldb::WriteOptions(), key, value);
    return status.ok();
  }

  bool Erase(const string& key, bool write_batch) {
    if (write_batch) {
      batch.Delete(key);
      return true;
    }
    auto status = ptr->Delete(leveldb::WriteOptions(), key);
    return status.ok();
  }

  void ClearBatch() { batch.Clear(); }

  bool CommitBatch() {
    auto status = ptr->Write(leveldb::WriteOptions(), &batch);
    return status.ok();
  }
};

// 蓬萊：使用者詞條的記憶體副本。UserDictionary::Lookup 每個鍵、每個起點都做一次深度到
// max_user_phrase_syllables 的 DFS，每個前綴一次 Jump；LevelDB 的 Seek（合併 memtable、索引區塊、
// 區塊內二分加線性解碼）一次幾 µs，學習紀錄一萬多筆時佔掉每鍵一半以上的時間。副本是照 key 排好的陣列，
// Jump 是一次 lower_bound。std::string 的比較跟 LevelDB 的 BytewiseComparator 一樣逐位元組（unsigned），
// 順序相同；只放 key ≥ " " 的詞條，metadata（\x01 開頭）不放，Lookup 一開始就 Jump(" ") 跳過它們
struct LevelDbMirror {
  vector<pair<string, string>> rows;
};

static bool mirror_row_less(const pair<string, string>& row, const string& key) {
  return row.first < key;
}

class LevelDbMirrorAccessor : public DbAccessor {
 public:
  explicit LevelDbMirrorAccessor(an<LevelDbMirror> mirror)
      : DbAccessor(""), mirror_(std::move(mirror)) {
    Reset();
  }

  bool Reset() override { return Jump(prefix_); }

  // 結果等於整個陣列的 lower_bound。DfsLookup 照音節順序往後跳，目標多半就是剛讀過的那筆、
  // 或在後面不遠（不存在的前綴落在同一筆上）：先用目前位置縮小範圍，往後用指數搜尋
  bool Jump(const string& key) override {
    const auto& rows = mirror_->rows;
    const size_t n = rows.size();
    size_t lo = 0, hi = n;
    if (pos_ > 0 && pos_ <= n) {
      if (!(rows[pos_ - 1].first < key)) {  // 目標 ≤ 剛讀過的那筆
        if (pos_ == 1 || rows[pos_ - 2].first < key) {
          pos_ = pos_ - 1;
          return true;
        }
        hi = pos_ - 1;
      } else {  // 目標在後面：rows[< pos_] 都比它小
        lo = pos_;
        size_t step = 1;
        size_t probe = lo;
        while (probe < n && rows[probe].first < key) {
          lo = probe + 1;
          probe = lo + step;
          step <<= 1;
        }
        hi = (std::min)(probe, n);
      }
    }
    pos_ = std::lower_bound(rows.begin() + lo, rows.begin() + hi, key,
                            mirror_row_less) -
           rows.begin();
    return true;
  }

  bool GetNextRecord(string* key, string* value) override {
    const auto& rows = mirror_->rows;
    if (pos_ >= rows.size() || !key || !value)
      return false;
    *key = rows[pos_].first;
    if (!MatchesPrefix(*key))
      return false;
    *value = rows[pos_].second;
    ++pos_;
    return true;
  }

  bool exhausted() override {
    return pos_ >= mirror_->rows.size() ||
           !MatchesPrefix(mirror_->rows[pos_].first);
  }

 private:
  an<LevelDbMirror> mirror_;  // 查詢中副本被改時，改的那邊先複製一份（copy-on-write）
  size_t pos_ = 0;
};

// LevelDbAccessor members

LevelDbAccessor::LevelDbAccessor() {}

LevelDbAccessor::LevelDbAccessor(LevelDbCursor* cursor, const string& prefix)
    : DbAccessor(prefix),
      cursor_(cursor),
      is_metadata_query_(prefix == kMetaCharacter) {
  Reset();
}

LevelDbAccessor::~LevelDbAccessor() {
  cursor_->Release();
}

bool LevelDbAccessor::Reset() {
  return cursor_->Jump(prefix_);
}

bool LevelDbAccessor::Jump(const string& key) {
  return cursor_->Jump(key);
}

bool LevelDbAccessor::GetNextRecord(string* key, string* value) {
  if (!cursor_->IsValid() || !key || !value)
    return false;
  *key = cursor_->GetKey();
  if (!MatchesPrefix(*key)) {
    return false;
  }
  if (is_metadata_query_) {
    key->erase(0, 1);  // remove meta character
  }
  *value = cursor_->GetValue();
  cursor_->Next();
  return true;
}

bool LevelDbAccessor::exhausted() {
  return !cursor_->IsValid() || !MatchesPrefix(cursor_->GetKey());
}

// LevelDb members

LevelDb::LevelDb(const path& file_path,
                 const string& db_name,
                 const string& db_type)
    : Db(file_path, db_name), db_type_(db_type) {}

LevelDb::~LevelDb() {
  if (loaded())
    Close();
}

void LevelDb::Initialize() {
  db_.reset(new LevelDbWrapper);
}

an<DbAccessor> LevelDb::QueryMetadata() {
  return Query(kMetaCharacter);
}

an<DbAccessor> LevelDb::QueryAll() {
  an<DbAccessor> all = Query("");
  if (all)
    all->Jump(" ");  // skip metadata
  return all;
}

an<DbAccessor> LevelDb::Query(const string& key) {
  if (!loaded())
    return nullptr;
  return New<LevelDbAccessor>(db_->CreateCursor(), key);
}

bool LevelDb::Fetch(const string& key, string* value) {
  if (!value || !loaded())
    return false;
  return db_->Fetch(key, value);
}

bool LevelDb::Update(const string& key, const string& value) {
  if (!loaded() || readonly())
    return false;
  DLOG(INFO) << "update db entry: " << key << " => " << value;
  bool ok = db_->Update(key, value, in_transaction());
  if (!ok)
    MirrorReset();  // 不確定寫了沒，下次查詢從 LevelDB 重建
  else if (in_transaction())
    pending_.emplace_back(key, value);
  else
    MirrorApply(key, &value);
  return ok;
}

bool LevelDb::Erase(const string& key) {
  if (!loaded() || readonly())
    return false;
  DLOG(INFO) << "erase db entry: " << key;
  bool ok = db_->Erase(key, in_transaction());
  if (!ok)
    MirrorReset();
  else if (in_transaction())
    pending_.emplace_back(key, std::nullopt);
  else
    MirrorApply(key, nullptr);
  return ok;
}

an<DbAccessor> LevelDb::QueryUserEntries() {
  if (!loaded())
    return nullptr;
  if (!mirror_) {
    auto mirror = New<LevelDbMirror>();
    the<LevelDbCursor> cursor(db_->CreateCursor());
    for (cursor->Jump(" "); cursor->IsValid(); cursor->Next())
      mirror->rows.emplace_back(cursor->GetKey(), cursor->GetValue());
    cursor->Release();
    mirror->rows.shrink_to_fit();
    mirror_ = mirror;
  }
  return New<LevelDbMirrorAccessor>(mirror_);
}

void LevelDb::MirrorApply(const string& key, const string* value) {
  if (!mirror_ || key < " ")
    return;
  if (mirror_.use_count() > 1)
    mirror_ = New<LevelDbMirror>(*mirror_);
  auto& rows = mirror_->rows;
  auto it = std::lower_bound(rows.begin(), rows.end(), key, mirror_row_less);
  bool found = it != rows.end() && it->first == key;
  if (!value) {
    if (found)
      rows.erase(it);
  } else if (found) {
    it->second = *value;
  } else {
    rows.emplace(it, key, *value);
  }
}

// 不確定副本跟 LevelDB 一不一樣時丟掉，下次查詢重建。pending_ 不清：重建出來的是已經 commit 的內容，
// 交易中的寫入照樣要在 CommitTransaction 時套上去
void LevelDb::MirrorReset() {
  mirror_.reset();
}

bool LevelDb::Backup(const path& snapshot_file) {
  if (!loaded())
    return false;
  LOG(INFO) << "backing up db '" << name() << "' to " << snapshot_file;
  // TODO(chen): suppose we only use this method for user dbs.
  bool success = UserDbHelper(this).UniformBackup(snapshot_file);
  if (!success) {
    LOG(ERROR) << "failed to create snapshot file '" << snapshot_file
               << "' for db '" << name() << "'.";
  }
  return success;
}

bool LevelDb::Restore(const path& snapshot_file) {
  if (!loaded() || readonly())
    return false;
  // TODO(chen): suppose we only use this method for user dbs.
  bool success = UserDbHelper(this).UniformRestore(snapshot_file);
  MirrorReset();
  if (!success) {
    LOG(ERROR) << "failed to restore db '" << name() << "' from '"
               << snapshot_file << "'.";
  }
  return success;
}

bool LevelDb::Recover() {
  LOG(INFO) << "trying to recover db '" << name() << "'.";
  auto status = leveldb::RepairDB(file_path().string(), leveldb::Options());
  if (status.ok()) {
    LOG(INFO) << "repair finished.";
    return true;
  }
  LOG(ERROR) << "db recovery failed: " << status.ToString();
  return false;
}

bool LevelDb::Remove() {
  if (loaded()) {
    LOG(ERROR) << "attempt to remove opened db '" << name() << "'.";
    return false;
  }
  auto status = leveldb::DestroyDB(file_path().string(), leveldb::Options());
  if (!status.ok()) {
    LOG(ERROR) << "Error removing db '" << name() << "': " << status.ToString();
    return false;
  }
  return true;
}

bool LevelDb::Open() {
  if (loaded())
    return false;
  MirrorReset();
  pending_.clear();
  Initialize();
  readonly_ = false;
  auto status = db_->Open(file_path(), readonly_);
  loaded_ = status.ok();

  if (loaded_) {
    string db_name;
    if (!MetaFetch("/db_name", &db_name)) {
      if (!CreateMetadata()) {
        LOG(ERROR) << "error creating metadata.";
        Close();
      }
    }
  } else {
    LOG(ERROR) << "Error opening db '" << name() << "': " << status.ToString();
  }
  return loaded_;
}

bool LevelDb::OpenReadOnly() {
  if (loaded())
    return false;
  MirrorReset();
  pending_.clear();
  Initialize();
  readonly_ = true;
  auto status = db_->Open(file_path(), readonly_);
  loaded_ = status.ok();

  if (!loaded_) {
    LOG(ERROR) << "Error opening db '" << name() << "' read-only.";
  }
  return loaded_;
}

bool LevelDb::Close() {
  if (!loaded())
    return false;

  db_->Release();
  MirrorReset();
  pending_.clear();

  LOG(INFO) << "closed db '" << name() << "'.";
  loaded_ = false;
  readonly_ = false;
  in_transaction_ = false;
  return true;
}

bool LevelDb::CreateMetadata() {
  return Db::CreateMetadata() && MetaUpdate("/db_type", db_type_);
}

bool LevelDb::MetaFetch(const string& key, string* value) {
  return Fetch(kMetaCharacter + key, value);
}

bool LevelDb::MetaUpdate(const string& key, const string& value) {
  return Update(kMetaCharacter + key, value);
}

bool LevelDb::BeginTransaction() {
  if (!loaded())
    return false;
  db_->ClearBatch();
  pending_.clear();
  in_transaction_ = true;
  return true;
}

bool LevelDb::AbortTransaction() {
  if (!loaded() || !in_transaction())
    return false;
  db_->ClearBatch();
  pending_.clear();
  in_transaction_ = false;
  return true;
}

bool LevelDb::CommitTransaction() {
  if (!loaded() || !in_transaction())
    return false;
  bool ok = db_->CommitBatch();
  db_->ClearBatch();
  in_transaction_ = false;
  if (ok) {
    for (const auto& op : pending_)
      MirrorApply(op.first, op.second ? &*op.second : nullptr);
    pending_.clear();
  } else {
    MirrorReset();
    pending_.clear();
  }
  return ok;
}

template <>
RIME_DLL string UserDbComponent<LevelDb>::extension() const {
  return ".userdb";
}

template <>
RIME_DLL UserDbWrapper<LevelDb>::UserDbWrapper(const path& file_path,
                                               const string& db_name)
    : LevelDb(file_path, db_name, "userdb") {}

}  // namespace rime
