#include "engine/database.h"

#include <unordered_set>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "execution/executor.h"
#include "optimizer/optimizer.h"
#include "recovery/log_manager.h"
#include "recovery/recovery_manager.h"
#include "sql/parser.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/table_heap.h"
#include "txn/lock_manager.h"
#include "txn/transaction.h"

namespace minidb {

Database::Database(const std::string &path, size_t pool_frames)
    : path_(path), pool_frames_(pool_frames) { Open(); }

Database::~Database() {
  if (crashed_) return;  // mimic power loss: nothing flushed on a crashed instance
  if (active_txn_) Abort(active_txn_.get());  // roll back any open txn
  if (bpm_) bpm_->FlushAll();
  if (catalog_) catalog_->Persist();
  if (disk_) disk_->Flush();
}

void Database::Open() {
  disk_ = std::make_unique<DiskManager>(path_ + ".db");
  log_ = std::make_unique<LogManager>(path_ + ".wal");
  bpm_ = std::make_unique<BufferPool>(pool_frames_, disk_.get());
  bpm_->SetLogManager(log_.get());
  catalog_ = std::make_unique<Catalog>(bpm_.get(), log_.get(), path_ + ".meta");
  lock_mgr_ = std::make_unique<LockManager>();

  catalog_->Load();  // restore schemas (and rebuild indexes from heaps)
  RecoveryManager rm(log_.get(), catalog_.get(), bpm_.get());
  rm.Recover();      // ARIES-lite: redo committed, undo in-flight
}

void Database::SimulateCrash() {
  // Drop everything WITHOUT flushing dirty pages. The on-disk WAL survives.
  crashed_ = true;
  active_txn_.reset();
  catalog_.reset();
  bpm_.reset();
  log_.reset();
  disk_.reset();
}

void Database::Commit(Transaction *txn) {
  LogRecord c;
  c.type = LogType::COMMIT;
  c.txn_id = txn->id();
  c.prev_lsn = txn->prev_lsn();
  log_->Append(c);
  log_->Flush();             // force-log-at-commit (durability point)
  lock_mgr_->UnlockAll(txn); // strict 2PL: release all locks at end
  txn->set_state(TxnState::COMMITTED);
}

void Database::Abort(Transaction *txn) {
  // Undo this txn's heap changes in reverse order.
  std::unordered_set<oid_t> touched;
  auto &undo = txn->undo_log();
  for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
    TableInfo *ti = catalog_->GetTableByOid(it->table_oid);
    if (!ti) continue;
    if (it->type == LogType::INSERT) { ti->heap->RemoveTuple(it->rid); touched.insert(it->table_oid); }
    else if (it->type == LogType::MARK_DELETE) { ti->heap->RestoreTuple(it->rid, it->old_tuple); touched.insert(it->table_oid); }
  }
  // Indexes aren't logged -> rebuild touched tables' indexes from the heap.
  for (oid_t oid : touched) {
    TableInfo *ti = catalog_->GetTableByOid(oid);
    if (ti) { catalog_->RebuildAllIndexes(ti); catalog_->MarkStatsDirty(ti->name); }
  }
  LogRecord a;
  a.type = LogType::ABORT;
  a.txn_id = txn->id();
  a.prev_lsn = txn->prev_lsn();
  log_->Append(a);
  log_->Flush();
  lock_mgr_->UnlockAll(txn);
  txn->set_state(TxnState::ABORTED);
}

ExecutionResult Database::Execute(const std::string &sql) {
  ExecutionResult result;
  std::unique_ptr<Transaction> temp;
  Transaction *running = nullptr;
  bool autocommit = false;

  try {
    Statement stmt = Parser(sql).Parse();

    switch (stmt.type) {
      case StmtType::CREATE_TABLE: {
        std::vector<Column> cols;
        int pk = -1;
        for (size_t i = 0; i < stmt.columns.size(); i++) {
          const auto &c = stmt.columns[i];
          cols.push_back({c.name, c.type, c.length});
          if (c.is_pk) pk = static_cast<int>(i);
        }
        catalog_->CreateTable(stmt.table, Schema(cols, pk));
        result.message = "Table '" + stmt.table + "' created";
        return result;
      }
      case StmtType::CREATE_INDEX:
        catalog_->CreateIndex(stmt.index_name, stmt.table, stmt.index_col, false);
        result.message = "Index '" + stmt.index_name + "' created";
        return result;
      case StmtType::SHOW_TABLES:
        result.columns = {"table"};
        for (auto &n : catalog_->TableNames()) result.rows.push_back({n});
        return result;
      case StmtType::BEGIN: {
        if (active_txn_) throw DBException("already inside a transaction");
        active_txn_ = std::make_unique<Transaction>(next_txn_id_++);
        LogRecord b; b.type = LogType::BEGIN; b.txn_id = active_txn_->id();
        active_txn_->set_prev_lsn(log_->Append(b));
        result.message = "BEGIN (txn " + std::to_string(active_txn_->id()) + ")";
        return result;
      }
      case StmtType::COMMIT: {
        if (!active_txn_) throw DBException("no active transaction");
        Commit(active_txn_.get());
        result.message = "COMMIT (txn " + std::to_string(active_txn_->id()) + ")";
        active_txn_.reset();
        return result;
      }
      case StmtType::ABORT: {
        if (!active_txn_) throw DBException("no active transaction");
        Abort(active_txn_.get());
        result.message = "ROLLBACK (txn " + std::to_string(active_txn_->id()) + ")";
        active_txn_.reset();
        return result;
      }
      case StmtType::EXPLAIN: {
        ExecutorContext ctx{catalog_.get(), nullptr, nullptr};
        Optimizer opt(&ctx);
        CompiledPlan plan = opt.Plan(*stmt.inner);
        result.explain = plan.explain;
        result.message = "EXPLAIN";
        return result;
      }
      case StmtType::SELECT:
      case StmtType::INSERT:
      case StmtType::DELETE: {
        if (active_txn_) {
          running = active_txn_.get();
        } else {
          autocommit = true;
          temp = std::make_unique<Transaction>(next_txn_id_++);
          running = temp.get();
          LogRecord b; b.type = LogType::BEGIN; b.txn_id = running->id();
          running->set_prev_lsn(log_->Append(b));
        }
        ExecutorContext ctx{catalog_.get(), running, lock_mgr_.get()};
        Optimizer opt(&ctx);
        CompiledPlan plan = opt.Plan(stmt);

        plan.root->Init();
        if (plan.is_dml) {
          result.affected = plan.root->GetAffected();
          result.message = (stmt.type == StmtType::INSERT ? "Inserted " : "Deleted ") +
                           std::to_string(result.affected) + " row(s)";
        } else {
          result.columns = plan.column_names;
          Tuple t; RID r;
          while (plan.root->Next(&t, &r)) {
            std::vector<std::string> row;
            for (size_t i = 0; i < t.size(); i++) row.push_back(t.value(i).ToString());
            result.rows.push_back(std::move(row));
          }
        }
        if (autocommit) Commit(running);
        return result;
      }
      default:
        throw DBException("unsupported statement");
    }
  } catch (const TransactionAbortException &e) {
    if (running) { Abort(running); if (!autocommit) active_txn_.reset(); }
    result.ok = false;
    result.message = e.what();
    return result;
  } catch (const std::exception &e) {
    if (running) { Abort(running); if (!autocommit) active_txn_.reset(); }
    result.ok = false;
    result.message = std::string("Error: ") + e.what();
    return result;
  }
}

}  // namespace minidb
