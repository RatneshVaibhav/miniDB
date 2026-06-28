#include "txn/lock_manager.h"

#include <functional>
#include <unordered_map>

#include "common/exception.h"
#include "txn/transaction.h"

namespace minidb {

bool LockManager::CanGrant(const RID &rid, txn_id_t txn, Mode mode) {
  auto &q = table_[rid].queue;
  for (auto &r : q) {
    if (!r.granted || r.txn == txn) continue;
    // A granted lock held by another txn conflicts unless both are SHARED.
    if (mode == Mode::EXCLUSIVE || r.mode == Mode::EXCLUSIVE) return false;
  }
  return true;
}

// Build the wait-for graph (waiter -> holder) from the lock table and return a
// victim if a cycle exists. The victim is the largest txn id in the cycle
// (youngest), which is a standard, simple, starvation-free-enough policy.
txn_id_t LockManager::DetectVictim() {
  std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> waits_for;
  for (auto &[rid, lq] : table_) {
    for (auto &w : lq.queue) {
      if (w.granted) continue;
      for (auto &h : lq.queue) {
        if (!h.granted || h.txn == w.txn) continue;
        if (w.mode == Mode::EXCLUSIVE || h.mode == Mode::EXCLUSIVE) {
          waits_for[w.txn].insert(h.txn);
        }
      }
    }
  }

  // DFS cycle detection; collect the nodes on a found cycle.
  std::unordered_set<txn_id_t> visiting, visited;
  std::vector<txn_id_t> stack;
  txn_id_t victim = INVALID_TXN_ID;

  std::function<bool(txn_id_t)> dfs = [&](txn_id_t u) -> bool {
    visiting.insert(u);
    stack.push_back(u);
    for (txn_id_t v : waits_for[u]) {
      if (visiting.count(v)) {
        // Found a cycle: everything from v to top of stack is on it.
        bool on = false;
        for (txn_id_t node : stack) {
          if (node == v) on = true;
          if (on) victim = std::max(victim, node);
        }
        return true;
      }
      if (!visited.count(v) && dfs(v)) return true;
    }
    visiting.erase(u);
    stack.pop_back();
    visited.insert(u);
    return false;
  };

  for (auto &[u, _] : waits_for) {
    if (!visited.count(u)) {
      visiting.clear();
      stack.clear();
      if (dfs(u)) return victim;
    }
  }
  return INVALID_TXN_ID;
}

bool LockManager::Acquire(Transaction *txn, const RID &rid, Mode mode) {
  std::unique_lock<std::mutex> lk(latch_);
  auto &q = table_[rid].queue;

  // Already holding a sufficient lock?
  for (auto &r : q) {
    if (r.txn == txn->id() && r.granted) {
      if (r.mode == Mode::EXCLUSIVE || mode == Mode::SHARED) return true;  // strong enough
      // upgrade S -> X: wait until sole holder, then promote in place.
      while (!CanGrant(rid, txn->id(), Mode::EXCLUSIVE)) {
        txn_id_t v = DetectVictim();
        if (v != INVALID_TXN_ID) {
          aborted_.insert(v);
          cv_.notify_all();
          if (v == txn->id()) { aborted_.erase(v); throw TransactionAbortException("deadlock victim"); }
        }
        cv_.wait(lk);
        if (aborted_.count(txn->id())) { aborted_.erase(txn->id()); throw TransactionAbortException("deadlock victim"); }
      }
      r.mode = Mode::EXCLUSIVE;
      txn->exclusive_locks().insert(rid);
      return true;
    }
  }

  q.push_back({txn->id(), mode, false});
  auto self = std::prev(q.end());

  while (!CanGrant(rid, txn->id(), mode)) {
    txn_id_t v = DetectVictim();
    if (v != INVALID_TXN_ID) {
      aborted_.insert(v);
      cv_.notify_all();
      if (v == txn->id()) {
        table_[rid].queue.erase(self);
        aborted_.erase(v);
        throw TransactionAbortException("deadlock victim");
      }
    }
    cv_.wait(lk);
    if (aborted_.count(txn->id())) {
      table_[rid].queue.erase(self);
      aborted_.erase(txn->id());
      throw TransactionAbortException("deadlock victim");
    }
  }
  self->granted = true;
  if (mode == Mode::SHARED) txn->shared_locks().insert(rid);
  else txn->exclusive_locks().insert(rid);
  return true;
}

bool LockManager::LockShared(Transaction *txn, const RID &rid) { return Acquire(txn, rid, Mode::SHARED); }
bool LockManager::LockExclusive(Transaction *txn, const RID &rid) { return Acquire(txn, rid, Mode::EXCLUSIVE); }

void LockManager::UnlockAll(Transaction *txn) {
  std::lock_guard<std::mutex> lk(latch_);
  for (auto &[rid, lq] : table_) {
    lq.queue.remove_if([&](const Request &r) { return r.txn == txn->id(); });
  }
  txn->shared_locks().clear();
  txn->exclusive_locks().clear();
  cv_.notify_all();
}

}  // namespace minidb
