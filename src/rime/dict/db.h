//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2013-04-16 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_DB_H_
#define RIME_DB_H_

#include <rime_api.h>
#include <rime/common.h>
#include <rime/component.h>

namespace rime {

class DbAccessor {
 public:
  DbAccessor() = default;
  explicit DbAccessor(const string& prefix) : prefix_(prefix) {}
  virtual ~DbAccessor() = default;

  virtual bool Reset() = 0;
  virtual bool Jump(const string& key) = 0;
  virtual bool GetNextRecord(string* key, string* value) = 0;
  // 蓬萊：同 GetNextRecord，只是不複製：*key、*value 指著讀到的那一筆，到下一次讀、Jump、Reset 之前都有效。
  // 預設複製到 accessor 自己的緩衝區；學習紀錄的記憶體副本（level_db.cc）直接指著副本那一筆
  virtual bool GetNextRecordRef(const string** key, const string** value) {
    if (!GetNextRecord(&ref_key_, &ref_value_))
      return false;
    *key = &ref_key_;
    *value = &ref_value_;
    return true;
  }
  virtual bool exhausted() = 0;
  // 蓬萊：學習紀錄的記憶體副本：Jump、GetNextRecord 走的那個照 key 排好的陣列（到 accessor 釋放之前都有效，
  // 不會被改）；其他 accessor、或有前綴限制的是 nullptr
  virtual const vector<pair<string, string>>* SortedRecords() { return nullptr; }
  // 蓬萊：下一次 GetNextRecord 從第 index 筆讀；RecordIndex 是現在的下一筆（只有 SortedRecords 不是 nullptr 的才支援）
  virtual void SeekRecord(size_t index) {}
  virtual size_t RecordIndex() { return 0; }

 protected:
  bool MatchesPrefix(const string& key);

  string prefix_;
  string ref_key_;
  string ref_value_;
};

class Db : public Class<Db, const string&> {
 public:
  Db(const path& file_path, const string& name);
  virtual ~Db() = default;

  RIME_DLL bool Exists() const;
  RIME_DLL virtual bool Remove();

  virtual bool Open() = 0;
  virtual bool OpenReadOnly() = 0;
  virtual bool Close() = 0;

  virtual bool Backup(const path& snapshot_file) = 0;
  virtual bool Restore(const path& snapshot_file) = 0;

  virtual bool CreateMetadata();
  virtual bool MetaFetch(const string& key, string* value) = 0;
  virtual bool MetaUpdate(const string& key, const string& value) = 0;

  virtual an<DbAccessor> QueryMetadata() = 0;
  virtual an<DbAccessor> QueryAll() = 0;
  virtual an<DbAccessor> Query(const string& key) = 0;
  virtual bool Fetch(const string& key, string* value) = 0;
  virtual bool Update(const string& key, const string& value) = 0;
  virtual bool Erase(const string& key) = 0;

  const string& name() const { return name_; }
  const path& file_path() const { return file_path_; }
  bool loaded() const { return loaded_; }
  bool readonly() const { return readonly_; }
  bool disabled() const { return disabled_; }
  void disable() { disabled_ = true; }
  void enable() { disabled_ = false; }

 protected:
  string name_;
  path file_path_;
  bool loaded_ = false;
  bool readonly_ = false;
  bool disabled_ = false;
};

class Transactional {
 public:
  Transactional() = default;
  virtual ~Transactional() = default;
  virtual bool BeginTransaction() { return false; }
  virtual bool AbortTransaction() { return false; }
  virtual bool CommitTransaction() { return false; }
  bool in_transaction() const { return in_transaction_; }

 protected:
  bool in_transaction_ = false;
};

class Recoverable {
 public:
  virtual ~Recoverable() = default;
  virtual bool Recover() = 0;
};

class ResourceResolver;

class RIME_DLL DbComponentBase {
 public:
  DbComponentBase();
  virtual ~DbComponentBase();

  path DbFilePath(const string& name, const string& extension) const;

 protected:
  the<ResourceResolver> db_resource_resolver_;
};

template <class DbClass>
class DbComponent : public DbClass::Component, protected DbComponentBase {
 public:
  virtual string extension() const;

  DbClass* Create(const string& name) override {
    return new DbClass(DbFilePath(name, extension()), name);
  }
};

}  // namespace rime

#endif  // RIME_DB_H_
