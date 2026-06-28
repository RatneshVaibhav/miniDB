#pragma once
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "storage/lru_replacer.h"
#include "storage/page.h"

namespace minidb {

class DiskManager;
class LogManager;

// Fixed-size buffer pool with an LRU replacer. Hands out pinned Page* frames;
// callers must UnpinPage when done. Enforces the write-ahead rule by flushing the
// log up to a page's LSN before that dirty page is written back (when a
// LogManager is attached).
class BufferPool {
 public:
  BufferPool(size_t num_frames, DiskManager *disk);
  ~BufferPool();

  void SetLogManager(LogManager *lm) { log_manager_ = lm; }

  Page *FetchPage(page_id_t page_id);            // load + pin (creates if needed)
  Page *NewPage(page_id_t *page_id);             // allocate fresh page + pin
  bool UnpinPage(page_id_t page_id, bool is_dirty);
  bool FlushPage(page_id_t page_id);
  void FlushAll();

  // Stats for benchmarking / demos.
  size_t hits() const { return hits_; }
  size_t misses() const { return misses_; }

 private:
  frame_id_t GetVictimFrame();  // free frame or evicted; assumes latch held

  size_t num_frames_;
  DiskManager *disk_;
  LogManager *log_manager_{nullptr};

  std::vector<Page> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_list_;
  LRUReplacer replacer_;
  std::mutex latch_;

  size_t hits_{0};
  size_t misses_{0};
};

}  // namespace minidb
