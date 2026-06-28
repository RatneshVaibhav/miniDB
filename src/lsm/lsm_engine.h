#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lsm/sstable.h"

namespace minidb {

// Log-Structured Merge tree key-value store (Track C extension). Writes land in
// an in-memory MemTable; when it fills it is flushed to an immutable SSTable.
// Reads check the MemTable, then SSTables newest-to-oldest (bloom-gated).
// Size-tiered compaction merges SSTables, dropping obsolete versions/tombstones
// to bound read amplification and reclaim space.
class LSMEngine {
 public:
  // memtable_limit: entries before a flush. compaction_trigger: #SSTables before merge.
  explicit LSMEngine(const std::string &dir, size_t memtable_limit = 1000,
                     size_t compaction_trigger = 4);

  void Put(const std::string &key, const std::string &value);
  void Delete(const std::string &key);
  std::optional<std::string> Get(const std::string &key);

  void Flush();      // force the active memtable to an SSTable
  void Compact();    // merge all SSTables into one (full size-tiered compaction)

  // ---- stats for benchmarking ----
  size_t num_sstables() const { return sstables_.size(); }
  uint64_t total_sstable_bytes() const;
  size_t memtable_size() const { return memtable_.size(); }

 private:
  void MaybeFlush();
  std::string NextPath();

  std::string dir_;
  size_t memtable_limit_;
  size_t compaction_trigger_;
  std::map<std::string, LsmEntry> memtable_;       // sorted active table
  std::vector<std::unique_ptr<SSTable>> sstables_;  // newest at front
  int next_id_{0};
};

}  // namespace minidb
