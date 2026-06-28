#pragma once
#include <list>
#include <mutex>
#include <unordered_map>

#include "common/config.h"

namespace minidb {

// Classic LRU replacement policy over *unpinned* frames. A frame becomes a
// victim candidate when its pin count drops to zero (Unpin), and is removed
// from consideration when re-pinned (Pin).
class LRUReplacer {
 public:
  // Pick the least-recently-unpinned frame. Returns false if none available.
  bool Victim(frame_id_t *out) {
    std::lock_guard<std::mutex> g(latch_);
    if (lru_.empty()) return false;
    *out = lru_.back();          // back = least recently used
    table_.erase(lru_.back());
    lru_.pop_back();
    return true;
  }

  // Remove a frame from the replacer (it is now pinned / in use).
  void Pin(frame_id_t f) {
    std::lock_guard<std::mutex> g(latch_);
    auto it = table_.find(f);
    if (it == table_.end()) return;
    lru_.erase(it->second);
    table_.erase(it);
  }

  // Mark a frame as a victim candidate (just got unpinned).
  void Unpin(frame_id_t f) {
    std::lock_guard<std::mutex> g(latch_);
    if (table_.count(f)) return;        // already a candidate
    lru_.push_front(f);                 // front = most recently used
    table_[f] = lru_.begin();
  }

  size_t Size() {
    std::lock_guard<std::mutex> g(latch_);
    return lru_.size();
  }

 private:
  std::list<frame_id_t> lru_;
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> table_;
  std::mutex latch_;
};

}  // namespace minidb
