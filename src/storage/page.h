#pragma once
#include <cstring>
#include <shared_mutex>

#include "common/config.h"

namespace minidb {

// A fixed-size in-memory page frame managed by the BufferPool. The raw bytes in
// data_ are interpreted by higher layers (TablePage for heap tuples, B+Tree node
// pages for the index). Pin count and dirty flag are buffer-pool bookkeeping.
class Page {
 public:
  Page() { ResetMemory(); }

  char *GetData() { return data_; }
  const char *GetData() const { return data_; }
  page_id_t GetPageId() const { return page_id_; }
  int GetPinCount() const { return pin_count_; }
  bool IsDirty() const { return is_dirty_; }

  void ResetMemory() { std::memset(data_, 0, PAGE_SIZE); }

  // Page LSN lives in the first 8 bytes of every page so recovery can compare it
  // against log records during redo.
  lsn_t GetLSN() const { lsn_t l; std::memcpy(&l, data_, sizeof(l)); return l; }
  void SetLSN(lsn_t lsn) { std::memcpy(data_, &lsn, sizeof(lsn)); }

 private:
  friend class BufferPool;
  char data_[PAGE_SIZE];
  page_id_t page_id_{INVALID_PAGE_ID};
  int pin_count_{0};
  bool is_dirty_{false};
  std::shared_mutex latch_;  // page-level latch (used by B+Tree crabbing-lite / safety)
};

}  // namespace minidb
