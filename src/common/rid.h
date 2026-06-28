#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "common/config.h"

namespace minidb {

// Record identifier: locates a tuple by (page, slot). Also used as the lock
// granule for the 2PL lock manager.
struct RID {
  page_id_t page_id{INVALID_PAGE_ID};
  slot_id_t slot{-1};

  RID() = default;
  RID(page_id_t p, slot_id_t s) : page_id(p), slot(s) {}

  bool operator==(const RID &o) const { return page_id == o.page_id && slot == o.slot; }
  bool operator<(const RID &o) const {
    return page_id != o.page_id ? page_id < o.page_id : slot < o.slot;
  }
  bool valid() const { return page_id != INVALID_PAGE_ID && slot >= 0; }
  std::string ToString() const {
    return "(" + std::to_string(page_id) + "," + std::to_string(slot) + ")";
  }
};

}  // namespace minidb

namespace std {
template <>
struct hash<minidb::RID> {
  size_t operator()(const minidb::RID &r) const {
    return (static_cast<size_t>(r.page_id) << 16) ^ static_cast<size_t>(r.slot);
  }
};
}  // namespace std
