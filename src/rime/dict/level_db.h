//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2014-12-04 Chen Gong <chen.sst@gmail.com>
//
#ifndef RIME_LEVEL_DB_H_
#define RIME_LEVEL_DB_H_

#include <optional>
#include <rime/dict/db.h>

namespace rime {

struct LevelDbCursor;
struct LevelDbWrapper;
struct LevelDbMirror;

class LevelDb;

class LevelDbAccessor : public DbAccessor {
 public:
  LevelDbAccessor();
  LevelDbAccessor(LevelDbCursor* cursor, const string& prefix);
  virtual ~LevelDbAccessor();

  bool Reset() override;
  bool Jump(const string& key) override;
  bool GetNextRecord(string* key, string* value) override;
  bool exhausted() override;

 private:
  the<LevelDbCursor> cursor_;
  bool is_metadata_query_ = false;
};

class LevelDb : public Db, public Recoverable, public Transactional {
 public:
  LevelDb(const path& file_path,
          const string& db_name,
          const string& db_type = "");
  virtual ~LevelDb();

  bool Remove() override;
  bool Open() override;
  bool OpenReadOnly() override;
  bool Close() override;

  bool Backup(const path& snapshot_file) override;
  bool Restore(const path& snapshot_file) override;

  bool CreateMetadata() override;
  bool MetaFetch(const string& key, string* value) override;
  bool MetaUpdate(const string& key, const string& value) override;

  an<DbAccessor> QueryMetadata() override;
  an<DbAccessor> QueryAll() override;
  an<DbAccessor> Query(const string& key) override;
  bool Fetch(const string& key, string* value) override;
  bool Update(const string& key, const string& value) override;
  bool Erase(const string& key) override;

  // Recoverable
  bool Recover() override;

  // Transactional
  bool BeginTransaction() override;
  bool AbortTransaction() override;
  bool CommitTransaction() override;

  // 蓬萊：使用者詞條（key ≥ " "，不含 metadata）的記憶體副本，給 UserDictionary::Lookup 逐鍵查。
  // 跟 Query("") 走 LevelDB 的結果相同：只看得到已經寫進 LevelDB 的（交易中的寫入 commit 之後才看得到）
  an<DbAccessor> QueryUserEntries();

 private:
  void Initialize();
  void MirrorApply(const string& key, const string* value);  // value 是 nullptr＝刪掉
  void MirrorReset();

  the<LevelDbWrapper> db_;
  string db_type_;
  an<LevelDbMirror> mirror_;
  // 交易中的寫入（照順序；second 沒值＝刪掉），CommitTransaction 成功才套到副本
  vector<pair<string, std::optional<string>>> pending_;
};

}  // namespace rime

#endif  // RIME_LEVEL_DB_H_
