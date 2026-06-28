#pragma once
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <unordered_set>

#include "common/rid.h"

namespace minidb {

class Transaction;

// Row-granularity lock manager implementing strict two-phase locking with
// shared/exclusive modes. Serializable isolation is achieved by holding all
// locks until commit/abort (UnlockAll). Deadlocks are resolved by wait-for
// graph cycle detection, aborting the youngest (highest-id) transaction.
class LockManager {
 public:
  enum class Mode { SHARED, EXCLUSIVE };

  // Acquire a lock; blocks until granted. Throws TransactionAbortException if
  // this transaction is chosen as a deadlock victim.
  bool LockShared(Transaction *txn, const RID &rid);
  bool LockExclusive(Transaction *txn, const RID &rid);

  // Release every lock held by txn (called at commit/abort).
  void UnlockAll(Transaction *txn);

 private:
  struct Request {
    txn_id_t txn;
    Mode mode;
    bool granted;
  };
  struct LockQueue {
    std::list<Request> queue;
  };

  bool Acquire(Transaction *txn, const RID &rid, Mode mode);
  bool CanGrant(const RID &rid, txn_id_t txn, Mode mode);
  // Returns a victim txn id if the wait-for graph has a cycle, else -1.
  txn_id_t DetectVictim();

  std::map<RID, LockQueue> table_;
  std::unordered_set<txn_id_t> aborted_;
  std::mutex latch_;
  std::condition_variable cv_;
};

}  // namespace minidb
