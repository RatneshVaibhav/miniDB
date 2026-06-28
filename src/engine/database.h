#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "common/config.h"

namespace minidb {

class DiskManager;
class BufferPool;
class LogManager;
class Catalog;
class LockManager;
class Transaction;

// Result of executing one SQL statement.
struct ExecutionResult {
  bool ok{true};
  std::string message;                            // status or error text
  std::vector<std::string> columns;               // result header (SELECT)
  std::vector<std::vector<std::string>> rows;      // stringified result rows
  size_t affected{0};                             // INSERT/DELETE row count
  std::string explain;                            // EXPLAIN output
};

// Top-level database: wires the storage, indexing, execution, transaction, and
// recovery subsystems together and runs SQL statements. Recovery runs on open.
class Database {
 public:
  // `path` is a base name; the engine creates <path>.db, <path>.wal, <path>.meta.
  // `pool_frames` sizes the buffer pool — shrink it to force page eviction
  // (stealing of dirty uncommitted pages) for the recovery demo.
  explicit Database(const std::string &path, size_t pool_frames = DEFAULT_POOL_FRAMES);
  ~Database();

  ExecutionResult Execute(const std::string &sql);

  // Crash simulation for the recovery demo: drop in-memory state WITHOUT
  // flushing dirty pages, mimicking power loss. The WAL on disk is preserved.
  void SimulateCrash();

  Catalog *catalog() { return catalog_.get(); }
  BufferPool *buffer_pool() { return bpm_.get(); }

 private:
  void Open();
  void BeginIfNeeded(bool &created_here);
  void Commit(Transaction *txn);
  void Abort(Transaction *txn);

  std::string path_;
  size_t pool_frames_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<LogManager> log_;
  std::unique_ptr<BufferPool> bpm_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<LockManager> lock_mgr_;

  std::atomic<txn_id_t> next_txn_id_{1};
  std::unique_ptr<Transaction> active_txn_;  // explicit BEGIN..COMMIT, else null
  bool crashed_{false};
};

}  // namespace minidb
