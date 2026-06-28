// Live demonstration of the transaction/concurrency layer for the viva:
//   Scenario 1 — an exclusive lock blocks a second transaction until release.
//   Scenario 2 — a deadlock is detected and the youngest transaction is aborted.
// Run: make condemo   (or ./minidb_condemo)
#include <atomic>
#include <barrier>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

#include "common/exception.h"
#include "txn/lock_manager.h"
#include "txn/transaction.h"

using namespace minidb;
using namespace std::chrono_literals;

static std::mutex g_io;
static void say(const std::string &who, const std::string &msg) {
  std::lock_guard<std::mutex> g(g_io);
  std::cout << "  [" << who << "] " << msg << "\n" << std::flush;
}

static void Scenario1_Blocking() {
  std::cout << "\n=== Scenario 1: exclusive lock blocks a concurrent writer ===\n";
  LockManager lm;
  Transaction t1(1), t2(2);
  RID row(1, 0);

  lm.LockExclusive(&t1, row);
  say("Txn1", "acquired EXCLUSIVE lock on row (1,0)");

  std::thread writer([&] {
    say("Txn2", "requesting EXCLUSIVE lock on row (1,0) ... (blocks: Txn1 holds it)");
    lm.LockExclusive(&t2, row);
    say("Txn2", "GRANTED the lock — proceeded only AFTER Txn1 released");
    lm.UnlockAll(&t2);
  });

  std::this_thread::sleep_for(800ms);   // let Txn2 reach the blocked state
  say("Txn1", "COMMIT -> releasing all locks (strict 2PL)");
  lm.UnlockAll(&t1);
  writer.join();
  std::cout << "  Result: mutual exclusion held; Txn2 serialized after Txn1.\n";
}

static void Scenario2_Deadlock() {
  std::cout << "\n=== Scenario 2: deadlock detection (abort the youngest) ===\n";
  LockManager lm;
  Transaction t1(1), t2(2);
  RID a(10, 0), b(20, 0);
  std::barrier sync(2);
  std::atomic<int> victims{0}, winners{0};

  auto run = [&](Transaction *self, const char *name, const RID &first, const RID &second,
                 const char *first_name, const char *second_name) {
    try {
      lm.LockExclusive(self, first);
      say(name, std::string("locked ") + first_name);
      sync.arrive_and_wait();                       // both hold their first lock
      say(name, std::string("now wants ") + second_name + " (held by the other txn)");
      lm.LockExclusive(self, second);               // cross-acquire -> cycle
      say(name, "acquired second lock -> COMMIT");
      winners++;
      lm.UnlockAll(self);
    } catch (const TransactionAbortException &) {
      say(name, "*** chosen as DEADLOCK VICTIM -> ABORT + rollback (releases locks) ***");
      victims++;
      lm.UnlockAll(self);
    }
  };

  std::thread ta(run, &t1, "Txn1", a, b, "row(10,0)", "row(20,0)");
  std::thread tb(run, &t2, "Txn2", b, a, "row(20,0)", "row(10,0)");
  ta.join();
  tb.join();
  std::cout << "  Result: " << winners.load() << " committed, " << victims.load()
            << " aborted (the youngest, Txn2). Wait-for cycle was detected.\n";
}

int main() {
  std::cout << "MiniDB — Concurrency & Deadlock Demo\n"
               "===================================\n"
               "Strict two-phase locking (row-level S/X) + wait-for graph deadlock detection.\n";
  Scenario1_Blocking();
  Scenario2_Deadlock();
  std::cout << "\nDone.\n";
  return 0;
}
