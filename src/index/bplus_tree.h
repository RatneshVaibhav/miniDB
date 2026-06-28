#pragma once
#include <functional>
#include <vector>

#include "common/config.h"
#include "common/rid.h"
#include "index/index_key.h"

namespace minidb {

class BufferPool;

// Disk-backed B+Tree mapping IndexKey -> RID, with leaf pages linked for range
// scans. Node pages live in the buffer pool / data file. The root page id is
// tracked in memory; indexes are rebuilt from the heap on startup, so the tree
// need not survive restarts on its own (documented design choice).
//
// Node page layout (over the raw 4096-byte page, LSN reserved in bytes 0..8):
//   off 8 : is_leaf (u8)
//   off 12: size    (u32) -- number of keys
//   off 16: next_leaf page_id (leaf only; range-scan link)
//   off 24: leaf     -> keys[size] (32B) then rids[size] (8B)
//           internal -> children[size+1] (4B) then keys[size] (32B)
class BPlusTree {
 public:
  static constexpr int LEAF_MAX = 100;       // max key/rid pairs per leaf
  static constexpr int INTERNAL_MAX = 110;   // max keys per internal node

  // `unique` indexes (primary keys) reject duplicate keys; non-unique ones
  // (secondary indexes) allow many rows to share a key value.
  BPlusTree(BufferPool *bpm, bool unique = true)
      : bpm_(bpm), unique_(unique) {}

  page_id_t root_page_id() const { return root_page_id_; }
  bool empty() const { return root_page_id_ == INVALID_PAGE_ID; }

  // Insert key->rid. For a unique index, returns false if the key already exists.
  bool Insert(const IndexKey &key, const RID &rid);
  // Point lookup of the first matching key. Returns true and sets *out if found.
  bool Search(const IndexKey &key, RID *out) const;
  // Remove the first entry with this key. Returns true if it existed.
  bool Remove(const IndexKey &key);
  // Remove the specific (key,rid) entry — used for non-unique secondary indexes.
  bool Remove(const IndexKey &key, const RID &rid);
  // Visit every rid with key in [low, high] in sorted order.
  void RangeScan(const IndexKey &low, const IndexKey &high,
                 const std::function<void(const IndexKey &, const RID &)> &cb) const;

 private:
  struct SplitResult { bool split{false}; IndexKey up_key; page_id_t new_page{INVALID_PAGE_ID}; };

  page_id_t FindLeaf(const IndexKey &key) const;       // descend (>=): point lookups/insert
  page_id_t FindLeafForScan(const IndexKey &key) const; // descend (>): leftmost leaf for a key
  SplitResult InsertInto(page_id_t node_id, const IndexKey &key, const RID &rid, bool *inserted);
  void StartNewRoot(const IndexKey &key, page_id_t left, page_id_t right);

  BufferPool *bpm_;
  bool unique_{true};
  page_id_t root_page_id_{INVALID_PAGE_ID};
};

}  // namespace minidb
