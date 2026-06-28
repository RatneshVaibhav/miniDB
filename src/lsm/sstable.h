#pragma once
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lsm/bloom_filter.h"

namespace minidb {

// One entry stored in the LSM: a value, or a tombstone marking a deletion.
struct LsmEntry {
  std::string value;
  bool tombstone{false};
};

// Immutable sorted string table on disk. File layout:
//   [magic u32][count u32]
//   count records, sorted by key:
//     [keylen u32][key][type u8 (0=value,1=tombstone)][vallen u32][value]
// On open the full key->offset index and a bloom filter are loaded in memory.
class SSTable {
 public:
  // Write a sorted map to a new SSTable file and return the opened handle.
  static std::unique_ptr<SSTable> Create(const std::string &path,
                                         const std::map<std::string, LsmEntry> &data);
  // Open an existing SSTable file (rebuilds the in-memory index + bloom).
  static std::unique_ptr<SSTable> Open(const std::string &path);

  // Look up a key. Returns nullopt if absent in this table; returns an entry
  // (possibly a tombstone) if present.
  std::optional<LsmEntry> Get(const std::string &key);

  // All entries in sorted order (used by compaction).
  std::vector<std::pair<std::string, LsmEntry>> ScanAll();

  size_t num_keys() const { return index_.size(); }
  uint64_t file_size() const { return file_size_; }
  const std::string &path() const { return path_; }

 private:
  std::string path_;
  std::ifstream in_;
  std::map<std::string, uint64_t> index_;  // key -> file offset of record
  std::unique_ptr<BloomFilter> bloom_;
  uint64_t file_size_{0};

  LsmEntry ReadAt(uint64_t offset);
};

}  // namespace minidb
