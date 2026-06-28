#include <atomic>
#include <barrier>
#include <chrono>
#include <thread>

#include "common/exception.h"
#include "txn/lock_manager.h"
#include "txn/transaction.h"
#include "test_harness.h"

using namespace minidb;
using namespace std::chrono_literals;

TEST(concurrency, shared_locks_are_compatible) {
  LockManager lm;
  Transaction t1(1), t2(2);
  RID r(1, 0);
  REQUIRE(lm.LockShared(&t1, r));
  REQUIRE(lm.LockShared(&t2, r));  // both granted, no blocking
  lm.UnlockAll(&t1);
  lm.UnlockAll(&t2);
}

TEST(concurrency, exclusive_lock_blocks_until_release) {
  LockManager lm;
  Transaction t1(1), t2(2);
  RID r(2, 0);
  REQUIRE(lm.LockExclusive(&t1, r));

  std::atomic<bool> acquired{false};
  std::thread worker([&] {
    lm.LockExclusive(&t2, r);  // must block while t1 holds X
    acquired = true;
    lm.UnlockAll(&t2);
  });

  std::this_thread::sleep_for(100ms);
  REQUIRE(!acquired.load());  // still blocked
  lm.UnlockAll(&t1);          // release -> worker proceeds
  worker.join();
  REQUIRE(acquired.load());
}

TEST(concurrency, deadlock_detected_one_victim) {
  LockManager lm;
  Transaction t1(1), t2(2);
  RID r1(10, 0), r2(20, 0);

  std::barrier sync(2);
  std::atomic<int> victims{0};
  std::atomic<int> winners{0};

  auto run = [&](Transaction *self, const RID &first, const RID &second) {
    try {
      lm.LockExclusive(self, first);
      sync.arrive_and_wait();          // ensure both hold their first lock
      lm.LockExclusive(self, second);  // cross-acquire -> deadlock
      winners++;
      lm.UnlockAll(self);
    } catch (const TransactionAbortException &) {
      victims++;
      lm.UnlockAll(self);              // victim releases so the winner proceeds
    }
  };

  std::thread a(run, &t1, r1, r2);
  std::thread b(run, &t2, r2, r1);
  a.join();
  b.join();

  REQUIRE_EQ(victims.load(), 1);  // exactly one aborted
  REQUIRE_EQ(winners.load(), 1);  // the other committed
}
