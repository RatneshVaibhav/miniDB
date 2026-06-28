#include <cstdio>
#include <string>

#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/lru_replacer.h"
#include "storage/page.h"
#include "storage/table_heap.h"
#include "storage/table_page.h"
#include "test_harness.h"

using namespace minidb;

static std::string TmpFile(const char *name) {
  return std::string("build/_test_") + name + ".db";
}

TEST(storage, slotted_page_insert_get_delete) {
  char buf[PAGE_SIZE];
  TablePage tp(buf);
  tp.Init();
  RID r;
  int s0 = tp.InsertTuple("hello", 5);
  int s1 = tp.InsertTuple("world!!", 7);
  REQUIRE_EQ(s0, 0);
  REQUIRE_EQ(s1, 1);
  std::string out;
  REQUIRE(tp.GetTuple(0, &out));
  REQUIRE_EQ(out, std::string("hello"));
  REQUIRE(tp.GetTuple(1, &out));
  REQUIRE_EQ(out, std::string("world!!"));
  REQUIRE(tp.DeleteTuple(0));
  REQUIRE(!tp.GetTuple(0, &out));
  // slot indices stay stable after delete (append-only)
  int s2 = tp.InsertTuple("again", 5);
  REQUIRE_EQ(s2, 2);
}

TEST(storage, disk_manager_roundtrip) {
  std::remove(TmpFile("disk").c_str());
  DiskManager dm(TmpFile("disk"));
  page_id_t p = dm.AllocatePage();
  char w[PAGE_SIZE];
  for (int i = 0; i < PAGE_SIZE; i++) w[i] = static_cast<char>(i & 0xff);
  dm.WritePage(p, w);
  char r[PAGE_SIZE];
  dm.ReadPage(p, r);
  REQUIRE_EQ(std::string(r, PAGE_SIZE), std::string(w, PAGE_SIZE));
}

TEST(storage, lru_replacer_order) {
  LRUReplacer lru;
  lru.Unpin(1);
  lru.Unpin(2);
  lru.Unpin(3);  // 1 is least-recently-used
  frame_id_t v;
  REQUIRE(lru.Victim(&v));
  REQUIRE_EQ(v, 1);
  REQUIRE(lru.Victim(&v));
  REQUIRE_EQ(v, 2);
  lru.Pin(3);             // 3 is now in use; no candidates remain
  REQUIRE(!lru.Victim(&v));
}

TEST(storage, buffer_pool_fetch_and_evict) {
  std::remove(TmpFile("bp").c_str());
  DiskManager dm(TmpFile("bp"));
  BufferPool bp(3, &dm);  // tiny pool to force eviction
  page_id_t ids[5];
  for (int i = 0; i < 5; i++) {
    Page *pg = bp.NewPage(&ids[i]);
    std::string msg = "page-" + std::to_string(i);
    std::memcpy(pg->GetData() + 64, msg.data(), msg.size());
    bp.UnpinPage(ids[i], true);  // unpinned -> evictable
  }
  // Re-fetch the first page; it must have been written through and read back.
  Page *pg = bp.FetchPage(ids[0]);
  REQUIRE_EQ(std::string(pg->GetData() + 64, 6), std::string("page-0"));
  bp.UnpinPage(ids[0], false);
}

TEST(storage, table_heap_insert_scan_delete) {
  std::remove(TmpFile("heap").c_str());
  DiskManager dm(TmpFile("heap"));
  BufferPool bp(16, &dm);
  page_id_t first = TableHeap::Create(&bp);
  TableHeap heap(&bp, first, 1);

  std::vector<RID> rids;
  for (int i = 0; i < 500; i++) {
    RID rid;
    REQUIRE(heap.InsertTuple("row-" + std::to_string(i), &rid, nullptr));
    rids.push_back(rid);
  }
  // Full scan should see all 500.
  int count = 0;
  for (auto it = heap.Begin(); it != heap.End(); ++it) ++count;
  REQUIRE_EQ(count, 500);

  // Delete half, rescan.
  for (size_t i = 0; i < rids.size(); i += 2) heap.DeleteTuple(rids[i], nullptr);
  count = 0;
  for (auto it = heap.Begin(); it != heap.End(); ++it) ++count;
  REQUIRE_EQ(count, 250);
}
