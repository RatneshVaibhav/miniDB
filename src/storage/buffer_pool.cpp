#include "storage/buffer_pool.h"

#include "common/exception.h"
#include "recovery/log_manager.h"
#include "storage/disk_manager.h"

namespace minidb {

BufferPool::BufferPool(size_t num_frames, DiskManager *disk)
    : num_frames_(num_frames), disk_(disk), frames_(num_frames) {
  for (frame_id_t i = 0; i < static_cast<frame_id_t>(num_frames); i++) {
    free_list_.push_back(i);
  }
}

BufferPool::~BufferPool() { FlushAll(); }

// Caller must hold latch_. Returns a usable frame, evicting if necessary.
frame_id_t BufferPool::GetVictimFrame() {
  if (!free_list_.empty()) {
    frame_id_t f = free_list_.back();
    free_list_.pop_back();
    return f;
  }
  frame_id_t victim;
  if (!replacer_.Victim(&victim)) {
    throw DBException("BufferPool: all frames pinned, cannot evict");
  }
  Page &p = frames_[victim];
  if (p.is_dirty_) {
    if (log_manager_) log_manager_->Flush(p.GetLSN());  // write-ahead rule
    disk_->WritePage(p.page_id_, p.data_);
    p.is_dirty_ = false;
  }
  page_table_.erase(p.page_id_);
  return victim;
}

Page *BufferPool::FetchPage(page_id_t page_id) {
  std::lock_guard<std::mutex> g(latch_);
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t f = it->second;
    frames_[f].pin_count_++;
    replacer_.Pin(f);
    ++hits_;
    return &frames_[f];
  }
  ++misses_;
  frame_id_t f = GetVictimFrame();
  Page &p = frames_[f];
  p.ResetMemory();
  disk_->ReadPage(page_id, p.data_);
  p.page_id_ = page_id;
  p.pin_count_ = 1;
  p.is_dirty_ = false;
  page_table_[page_id] = f;
  replacer_.Pin(f);
  return &p;
}

Page *BufferPool::NewPage(page_id_t *page_id) {
  std::lock_guard<std::mutex> g(latch_);
  page_id_t pid = disk_->AllocatePage();
  frame_id_t f = GetVictimFrame();
  Page &p = frames_[f];
  p.ResetMemory();
  p.page_id_ = pid;
  p.pin_count_ = 1;
  p.is_dirty_ = true;  // new page must be written
  page_table_[pid] = f;
  replacer_.Pin(f);
  *page_id = pid;
  return &p;
}

bool BufferPool::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> g(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;
  frame_id_t f = it->second;
  Page &p = frames_[f];
  if (p.pin_count_ <= 0) return false;
  if (is_dirty) p.is_dirty_ = true;
  if (--p.pin_count_ == 0) replacer_.Unpin(f);
  return true;
}

bool BufferPool::FlushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> g(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;
  Page &p = frames_[it->second];
  if (log_manager_) log_manager_->Flush(p.GetLSN());
  disk_->WritePage(page_id, p.data_);
  p.is_dirty_ = false;
  return true;
}

void BufferPool::FlushAll() {
  std::lock_guard<std::mutex> g(latch_);
  for (auto &[pid, f] : page_table_) {
    Page &p = frames_[f];
    if (p.is_dirty_) {
      if (log_manager_) log_manager_->Flush(p.GetLSN());
      disk_->WritePage(pid, p.data_);
      p.is_dirty_ = false;
    }
  }
}

}  // namespace minidb
