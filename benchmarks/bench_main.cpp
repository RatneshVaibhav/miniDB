// MiniDB benchmark harness.
//   1. LSM-tree vs B+Tree storage: write throughput, read latency, space amplification (Track C).
//   2. Index scan vs sequential scan query latency (optimizer benefit).
//   3. Buffer pool hit rate.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "engine/database.h"
#include "index/bplus_tree.h"
#include "lsm/lsm_engine.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

using namespace minidb;
using Clock = std::chrono::steady_clock;

static double SecondsSince(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

static std::string EncodeKey(int64_t x) {  // 8-byte big-endian for ordered keys
  std::string s(8, '\0');
  for (int i = 0; i < 8; i++) s[i] = static_cast<char>((x >> (8 * (7 - i))) & 0xff);
  return s;
}

static void BenchStorage(int N) {
  std::printf("\n=== 1. LSM-tree vs B+Tree storage (%d keys) ===\n", N);
  std::mt19937 rng(7);
  std::vector<int> probe(10000);
  for (auto &p : probe) p = rng() % N;

  // ---- B+Tree ----
  std::filesystem::remove("bench_bptree.db");
  double bpt_write, bpt_read;
  uint64_t bpt_bytes;
  {
    DiskManager dm("bench_bptree.db");
    BufferPool bp(1024, &dm);
    BPlusTree tree(&bp);
    auto t0 = Clock::now();
    for (int i = 0; i < N; i++) tree.Insert(IndexKey::FromValue(Value::BigInt(i)), RID(i, 0));
    bpt_write = SecondsSince(t0);
    bp.FlushAll();
    auto t1 = Clock::now();
    RID r;
    for (int p : probe) tree.Search(IndexKey::FromValue(Value::BigInt(p)), &r);
    bpt_read = SecondsSince(t1);
    bpt_bytes = std::filesystem::file_size("bench_bptree.db");
  }

  // ---- LSM ----
  std::filesystem::remove_all("bench_lsm");
  double lsm_write, lsm_read;
  uint64_t lsm_bytes;
  {
    LSMEngine lsm("bench_lsm", 50000, 4);
    auto t0 = Clock::now();
    for (int i = 0; i < N; i++) lsm.Put(EncodeKey(i), std::to_string(i));
    lsm.Flush();
    lsm_write = SecondsSince(t0);
    auto t1 = Clock::now();
    for (int p : probe) lsm.Get(EncodeKey(p));
    lsm_read = SecondsSince(t1);
    lsm_bytes = lsm.total_sstable_bytes();
  }

  std::printf("%-12s %16s %18s %16s\n", "engine", "write (Kops/s)", "read (us/op)", "size (KB)");
  std::printf("%-12s %16.1f %18.3f %16.1f\n", "B+Tree",
              N / bpt_write / 1000.0, bpt_read / probe.size() * 1e6, bpt_bytes / 1024.0);
  std::printf("%-12s %16.1f %18.3f %16.1f\n", "LSM-tree",
              N / lsm_write / 1000.0, lsm_read / probe.size() * 1e6, lsm_bytes / 1024.0);

  // ---- space amplification: rewrite all keys 5x, measure before/after compaction ----
  std::filesystem::remove_all("bench_lsm_amp");
  {
    LSMEngine lsm("bench_lsm_amp", N / 5 + 1, 1000);  // no auto-compaction
    for (int round = 0; round < 5; round++)
      for (int i = 0; i < N; i++) lsm.Put(EncodeKey(i), std::to_string(round));
    lsm.Flush();
    uint64_t before = lsm.total_sstable_bytes();
    size_t before_n = lsm.num_sstables();
    lsm.Compact();
    uint64_t after = lsm.total_sstable_bytes();
    std::printf("\nLSM space amplification (5x overwrite of %d keys):\n", N);
    std::printf("  before compaction: %.1f KB across %zu SSTables\n", before / 1024.0, before_n);
    std::printf("  after  compaction: %.1f KB across %zu SSTable (%.1fx reclaimed)\n",
                after / 1024.0, lsm.num_sstables(), after ? (double)before / after : 0.0);
  }
}

static void BenchQuery(int N) {
  std::printf("\n=== 2. Index scan vs sequential scan (%d rows) ===\n", N);
  std::filesystem::remove("bench_q.db");
  std::filesystem::remove("bench_q.wal");
  std::filesystem::remove("bench_q.meta");
  Database db("bench_q");
  db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
  for (int i = 0; i < N; i++)
    db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");

  std::mt19937 rng(3);
  const int Q = 2000;
  auto t0 = Clock::now();
  for (int i = 0; i < Q; i++) db.Execute("SELECT v FROM t WHERE id = " + std::to_string(rng() % N));
  double idx = SecondsSince(t0);
  auto t1 = Clock::now();
  for (int i = 0; i < Q; i++) db.Execute("SELECT id FROM t WHERE v = " + std::to_string(rng() % N));
  double seq = SecondsSince(t1);

  std::printf("  point query on PK (index scan):  %.1f us/query\n", idx / Q * 1e6);
  std::printf("  point query on non-key (seq scan): %.1f us/query\n", seq / Q * 1e6);
  std::printf("  speedup from index: %.1fx\n", seq / idx);
}

static void BenchBufferPool() {
  std::printf("\n=== 3. Buffer pool hit rate ===\n");
  std::filesystem::remove("bench_bp.db");
  DiskManager dm("bench_bp.db");
  BufferPool bp(64, &dm);          // small pool
  std::vector<page_id_t> ids;
  for (int i = 0; i < 200; i++) { page_id_t p; bp.NewPage(&p); bp.UnpinPage(p, true); ids.push_back(p); }
  std::mt19937 rng(1);
  for (int i = 0; i < 5000; i++) {  // skewed access to the first 50 pages
    page_id_t p = ids[rng() % 50];
    bp.FetchPage(p);
    bp.UnpinPage(p, false);
  }
  double total = bp.hits() + bp.misses();
  std::printf("  hits=%zu misses=%zu hit_rate=%.1f%%\n", bp.hits(), bp.misses(), 100.0 * bp.hits() / total);
}

int main(int argc, char **argv) {
  int N = (argc > 1) ? std::stoi(argv[1]) : 100000;
  std::printf("MiniDB Benchmark Suite (N=%d)\n============================\n", N);
  BenchStorage(N);
  BenchQuery(N / 5);
  BenchBufferPool();
  std::printf("\nDone.\n");
  return 0;
}
