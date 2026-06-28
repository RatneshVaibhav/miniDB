#pragma once
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "common/rid.h"
#include "recovery/log_record.h"

namespace minidb {

enum class TxnState { GROWING, SHRINKING, COMMITTED, ABORTED };
enum class IsolationLevel { SERIALIZABLE };

// Runtime state of a transaction: its lock set (for strict 2PL release at
// end), its WAL prev-LSN (per-txn undo chain), and an in-memory undo list used
// for immediate rollback on ABORT / deadlock victimization.
class Transaction {
 public:
  explicit Transaction(txn_id_t id, IsolationLevel iso = IsolationLevel::SERIALIZABLE)
      : id_(id), iso_(iso) {}

  txn_id_t id() const { return id_; }
  TxnState state() const { return state_; }
  void set_state(TxnState s) { state_ = s; }
  IsolationLevel isolation() const { return iso_; }

  lsn_t prev_lsn() const { return prev_lsn_; }
  void set_prev_lsn(lsn_t l) { prev_lsn_ = l; }

  // Locks currently held (strict 2PL releases them all at commit/abort).
  std::unordered_set<RID> &shared_locks() { return shared_; }
  std::unordered_set<RID> &exclusive_locks() { return exclusive_; }

  // Undo records appended in execution order; replayed in reverse on abort.
  std::vector<LogRecord> &undo_log() { return undo_log_; }

 private:
  txn_id_t id_;
  IsolationLevel iso_;
  TxnState state_{TxnState::GROWING};
  lsn_t prev_lsn_{INVALID_LSN};
  std::unordered_set<RID> shared_;
  std::unordered_set<RID> exclusive_;
  std::vector<LogRecord> undo_log_;
};

}  // namespace minidb
