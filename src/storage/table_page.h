#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "common/config.h"
#include "common/rid.h"

namespace minidb {

// Slotted-page interpretation of a raw page for heap-file tuple storage.
// Layout:
//   [0..8)   page LSN          (managed by Page::SetLSN, reserved here)
//   [8..12)  next_page_id      (heap-file chaining)
//   [12..16) num_slots
//   [16..20) free_space_ptr    (offset to top of free region; tuples grow down from PAGE_SIZE)
//   [20..]   slot directory: num_slots * Slot{offset:u32, size:u32}  (size==0 => deleted)
//   tuples are packed downward from PAGE_SIZE.
class TablePage {
 public:
  struct Slot { uint32_t offset; uint32_t size; };
  static constexpr uint32_t HEADER_SIZE = 20;
  static constexpr uint32_t SLOT_SIZE = sizeof(Slot);  // 8

  explicit TablePage(char *data) : data_(data) {}

  void Init() {
    SetNextPageId(INVALID_PAGE_ID);
    SetNumSlots(0);
    SetFreeSpacePtr(PAGE_SIZE);
  }

  // A zeroed (never-initialized) page has free_space_ptr == 0. During recovery
  // a page whose Init write was lost in a crash is re-initialized before use.
  bool Uninitialized() const { return GetFreeSpacePtr() == 0; }
  void InitIfNeeded() { if (Uninitialized()) Init(); }

  page_id_t GetNextPageId() const { return Read32(8); }
  void SetNextPageId(page_id_t p) { Write32(8, p); }
  uint32_t GetNumSlots() const { return Read32(12); }

  // Bytes available for a brand-new tuple (accounts for the extra slot needed).
  uint32_t FreeSpaceForInsert() const {
    uint32_t used_by_dir = HEADER_SIZE + GetNumSlots() * SLOT_SIZE;
    uint32_t fsp = GetFreeSpacePtr();
    if (fsp <= used_by_dir) return 0;
    return fsp - used_by_dir;
  }

  // Insert a tuple, returning its slot index, or -1 if it does not fit.
  // Append-only: slot indices are never reused, which keeps them stable so the
  // recovery manager can deterministically replay inserts to the same slot.
  int InsertTuple(const char *tuple, uint32_t size) {
    if (FreeSpaceForInsert() < size + SLOT_SIZE) return -1;
    uint32_t n = GetNumSlots();
    uint32_t new_fsp = GetFreeSpacePtr() - size;
    std::memcpy(data_ + new_fsp, tuple, size);
    SetFreeSpacePtr(new_fsp);
    SetSlot(n, {new_fsp, size});
    SetNumSlots(n + 1);
    return static_cast<int>(n);
  }

  // ---- Recovery helpers (physical redo/undo at a known slot) ----

  // Redo an insert: if the slot is already present we are idempotent; otherwise
  // it must be the next append position (guaranteed by ordered LSN replay).
  void RedoInsert(slot_id_t slot, const char *tuple, uint32_t size) {
    if (slot < static_cast<slot_id_t>(GetNumSlots())) return;  // already applied
    InsertTuple(tuple, size);
  }

  // Restore a tuple's bytes into an existing slot (undo of a delete). Allocates
  // fresh space for the before-image and repoints the slot at it.
  void RestoreTuple(slot_id_t slot, const char *tuple, uint32_t size) {
    if (FreeSpaceForInsert() < size) return;
    uint32_t new_fsp = GetFreeSpacePtr() - size;
    std::memcpy(data_ + new_fsp, tuple, size);
    SetFreeSpacePtr(new_fsp);
    SetSlot(slot, {new_fsp, size});
  }

  bool GetTuple(slot_id_t slot, std::string *out) const {
    if (slot < 0 || static_cast<uint32_t>(slot) >= GetNumSlots()) return false;
    Slot s = GetSlot(slot);
    if (s.size == 0) return false;  // deleted
    out->assign(data_ + s.offset, s.size);
    return true;
  }

  // Tombstone a slot (space is reclaimed only on full-page reorganization, which
  // we keep out of scope — documented as a limitation).
  bool DeleteTuple(slot_id_t slot) {
    if (slot < 0 || static_cast<uint32_t>(slot) >= GetNumSlots()) return false;
    Slot s = GetSlot(slot);
    if (s.size == 0) return false;
    SetSlot(slot, {0, 0});
    return true;
  }

  Slot GetSlot(uint32_t i) const {
    Slot s;
    s.offset = Read32(HEADER_SIZE + i * SLOT_SIZE);
    s.size = Read32(HEADER_SIZE + i * SLOT_SIZE + 4);
    return s;
  }

 private:
  uint32_t GetFreeSpacePtr() const { return Read32(16); }
  void SetFreeSpacePtr(uint32_t v) { Write32(16, v); }
  void SetNumSlots(uint32_t v) { Write32(12, v); }
  void SetSlot(uint32_t i, Slot s) {
    Write32(HEADER_SIZE + i * SLOT_SIZE, s.offset);
    Write32(HEADER_SIZE + i * SLOT_SIZE + 4, s.size);
  }

  uint32_t Read32(uint32_t off) const {
    uint32_t v; std::memcpy(&v, data_ + off, 4); return v;
  }
  void Write32(uint32_t off, uint32_t v) { std::memcpy(data_ + off, &v, 4); }

  char *data_;
};

}  // namespace minidb
