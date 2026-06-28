#include <algorithm>
#include <random>
#include <vector>

#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "test_harness.h"

using namespace minidb;

static IndexKey K(int64_t x) { return IndexKey::FromValue(Value::BigInt(x)); }

TEST(bplustree, insert_search_many_with_splits) {
  std::remove("build/_bpt1.db");
  DiskManager dm("build/_bpt1.db");
  BufferPool bp(64, &dm);
  BPlusTree tree(&bp);

  const int N = 5000;
  for (int i = 0; i < N; i++) REQUIRE(tree.Insert(K(i), RID(i, i % 7)));
  // duplicate insert rejected
  REQUIRE(!tree.Insert(K(42), RID(0, 0)));

  for (int i = 0; i < N; i++) {
    RID r;
    REQUIRE(tree.Search(K(i), &r));
    REQUIRE_EQ(r.page_id, i);
  }
  RID r;
  REQUIRE(!tree.Search(K(N + 1), &r));
}

TEST(bplustree, random_insert_then_lookup) {
  std::remove("build/_bpt2.db");
  DiskManager dm("build/_bpt2.db");
  BufferPool bp(128, &dm);
  BPlusTree tree(&bp);

  std::vector<int> keys(3000);
  for (int i = 0; i < 3000; i++) keys[i] = i;
  std::mt19937 rng(12345);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (int k : keys) REQUIRE(tree.Insert(K(k), RID(k, 0)));
  for (int i = 0; i < 3000; i++) {
    RID r;
    REQUIRE(tree.Search(K(i), &r));
    REQUIRE_EQ(r.page_id, i);
  }
}

TEST(bplustree, range_scan_sorted) {
  std::remove("build/_bpt3.db");
  DiskManager dm("build/_bpt3.db");
  BufferPool bp(64, &dm);
  BPlusTree tree(&bp);
  for (int i = 0; i < 1000; i++) tree.Insert(K(i), RID(i, 0));

  std::vector<int> seen;
  tree.RangeScan(K(100), K(199), [&](const IndexKey &, const RID &r) { seen.push_back(r.page_id); });
  REQUIRE_EQ(static_cast<int>(seen.size()), 100);
  REQUIRE_EQ(seen.front(), 100);
  REQUIRE_EQ(seen.back(), 199);
  for (size_t i = 1; i < seen.size(); i++) REQUIRE(seen[i - 1] < seen[i]);  // sorted
}

TEST(bplustree, remove_keys) {
  std::remove("build/_bpt4.db");
  DiskManager dm("build/_bpt4.db");
  BufferPool bp(64, &dm);
  BPlusTree tree(&bp);
  for (int i = 0; i < 500; i++) tree.Insert(K(i), RID(i, 0));
  for (int i = 0; i < 500; i += 2) REQUIRE(tree.Remove(K(i)));
  REQUIRE(!tree.Remove(K(0)));  // already gone
  RID r;
  for (int i = 0; i < 500; i++) {
    bool found = tree.Search(K(i), &r);
    REQUIRE_EQ(found, (i % 2 == 1));
  }
}

TEST(bplustree, non_unique_duplicate_keys) {
  std::remove("build/_bpt6.db");
  DiskManager dm("build/_bpt6.db");
  BufferPool bp(64, &dm);
  BPlusTree tree(&bp, /*unique=*/false);
  // Insert many rows sharing a few key values (spanning splits).
  for (int i = 0; i < 1000; i++) tree.Insert(K(i % 5), RID(i, 0));
  // Each of the 5 keys should map to 200 rids.
  int count = 0;
  tree.RangeScan(K(2), K(2), [&](const IndexKey &, const RID &) { count++; });
  REQUIRE_EQ(count, 200);
  // Remove a specific (key,rid) leaves the rest intact.
  REQUIRE(tree.Remove(K(2), RID(2, 0)));   // i=2 -> key 2, rid (2,0)
  count = 0;
  tree.RangeScan(K(2), K(2), [&](const IndexKey &, const RID &) { count++; });
  REQUIRE_EQ(count, 199);
}

TEST(bplustree, varchar_keys) {
  std::remove("build/_bpt5.db");
  DiskManager dm("build/_bpt5.db");
  BufferPool bp(32, &dm);
  BPlusTree tree(&bp);
  std::vector<std::string> words = {"apple", "banana", "cherry", "date", "elder", "fig", "grape"};
  for (size_t i = 0; i < words.size(); i++)
    tree.Insert(IndexKey::FromValue(Value::Varchar(words[i])), RID(static_cast<int>(i), 0));
  RID r;
  REQUIRE(tree.Search(IndexKey::FromValue(Value::Varchar("cherry")), &r));
  REQUIRE_EQ(r.page_id, 2);
  REQUIRE(!tree.Search(IndexKey::FromValue(Value::Varchar("kiwi")), &r));
}
