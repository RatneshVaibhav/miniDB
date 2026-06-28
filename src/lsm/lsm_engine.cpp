#include "lsm/lsm_engine.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <filesystem>

namespace minidb {

LSMEngine::LSMEngine(const std::string &dir, size_t memtable_limit, size_t compaction_trigger)
    : dir_(dir), memtable_limit_(memtable_limit), compaction_trigger_(compaction_trigger) {
  std::filesystem::create_directories(dir_);
}

std::string LSMEngine::NextPath() {
  return dir_ + "/sst_" + std::to_string(next_id_++) + ".sst";
}

void LSMEngine::Put(const std::string &key, const std::string &value) {
  memtable_[key] = LsmEntry{value, false};
  MaybeFlush();
}

void LSMEngine::Delete(const std::string &key) {
  memtable_[key] = LsmEntry{"", true};  // tombstone
  MaybeFlush();
}

std::optional<std::string> LSMEngine::Get(const std::string &key) {
  // 1. active memtable (most recent)
  auto it = memtable_.find(key);
  if (it != memtable_.end()) {
    if (it->second.tombstone) return std::nullopt;
    return it->second.value;
  }
  // 2. SSTables, newest to oldest
  for (auto &sst : sstables_) {
    auto e = sst->Get(key);
    if (e.has_value()) {
      if (e->tombstone) return std::nullopt;
      return e->value;
    }
  }
  return std::nullopt;
}

void LSMEngine::MaybeFlush() {
  if (memtable_.size() >= memtable_limit_) Flush();
}

void LSMEngine::Flush() {
  if (memtable_.empty()) return;
  auto sst = SSTable::Create(NextPath(), memtable_);
  sstables_.insert(sstables_.begin(), std::move(sst));  // newest at front
  memtable_.clear();
  if (sstables_.size() > compaction_trigger_) Compact();
}

void LSMEngine::Compact() {
  if (sstables_.size() <= 1) return;
  // Merge newest-to-oldest, keeping the first (newest) version of each key and
  // dropping tombstones in the merged (bottom-level) output to reclaim space.
  std::map<std::string, LsmEntry> merged;
  for (auto &sst : sstables_) {            // front = newest
    for (auto &[k, e] : sst->ScanAll()) {
      if (merged.find(k) == merged.end()) merged[k] = e;  // newer wins
    }
  }
  std::map<std::string, LsmEntry> compacted;
  for (auto &[k, e] : merged) {
    if (!e.tombstone) compacted[k] = e;    // drop deletions at the bottom level
  }
  // Remove old files, write the single compacted SSTable.
  std::vector<std::string> old;
  for (auto &sst : sstables_) old.push_back(sst->path());
  sstables_.clear();
  for (auto &p : old) std::filesystem::remove(p);

  if (!compacted.empty()) {
    auto sst = SSTable::Create(NextPath(), compacted);
    sstables_.insert(sstables_.begin(), std::move(sst));
  }
}

uint64_t LSMEngine::total_sstable_bytes() const {
  uint64_t total = 0;
  for (auto &sst : sstables_) total += sst->file_size();
  return total;
}

}  // namespace minidb
