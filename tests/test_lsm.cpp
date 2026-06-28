#include <filesystem>
#include <string>

#include "lsm/lsm_engine.h"
#include "test_harness.h"

using namespace minidb;

static void CleanDir(const std::string &d) { std::filesystem::remove_all(d); }

TEST(lsm, put_get_overwrite_delete) {
  CleanDir("build/_lsm1");
  LSMEngine e("build/_lsm1", 100, 4);
  e.Put("a", "1");
  e.Put("b", "2");
  REQUIRE_EQ(e.Get("a").value(), std::string("1"));
  e.Put("a", "10");                          // overwrite in memtable
  REQUIRE_EQ(e.Get("a").value(), std::string("10"));
  e.Delete("b");
  REQUIRE(!e.Get("b").has_value());          // tombstoned
  REQUIRE(!e.Get("missing").has_value());
}

TEST(lsm, flush_creates_sstables_and_reads_persist) {
  CleanDir("build/_lsm2");
  LSMEngine e("build/_lsm2", 50, 100);       // flush every 50, no compaction
  for (int i = 0; i < 500; i++) e.Put("k" + std::to_string(i), "v" + std::to_string(i));
  REQUIRE(e.num_sstables() >= 1);
  // values in older SSTables still readable
  REQUIRE_EQ(e.Get("k0").value(), std::string("v0"));
  REQUIRE_EQ(e.Get("k499").value(), std::string("v499"));
}

TEST(lsm, newest_version_wins_across_sstables) {
  CleanDir("build/_lsm3");
  LSMEngine e("build/_lsm3", 10, 100);
  for (int i = 0; i < 10; i++) e.Put("x", "old" + std::to_string(i));  // forces a flush
  e.Flush();
  e.Put("x", "new");                          // newer version in a later memtable/sstable
  REQUIRE_EQ(e.Get("x").value(), std::string("new"));
}

TEST(lsm, compaction_merges_and_reclaims) {
  CleanDir("build/_lsm4");
  LSMEngine e("build/_lsm4", 100, 3);         // compact after 3 SSTables
  // Write the same 100 keys many times -> lots of obsolete versions.
  for (int round = 0; round < 10; round++) {
    for (int i = 0; i < 100; i++) e.Put("k" + std::to_string(i), "r" + std::to_string(round));
  }
  e.Flush();
  // After compaction the live set is just the 100 newest keys.
  REQUIRE(e.num_sstables() <= 3);
  for (int i = 0; i < 100; i++) REQUIRE(e.Get("k" + std::to_string(i)).has_value());
  // deletions dropped at bottom level
  e.Delete("k0");
  e.Flush();
  e.Compact();
  REQUIRE(!e.Get("k0").has_value());
}

TEST(lsm, bloom_filter_avoids_false_negatives) {
  CleanDir("build/_lsm5");
  LSMEngine e("build/_lsm5", 20, 100);
  for (int i = 0; i < 200; i++) e.Put("key" + std::to_string(i), std::to_string(i));
  e.Flush();
  for (int i = 0; i < 200; i++) REQUIRE(e.Get("key" + std::to_string(i)).has_value());  // no false negatives
}
