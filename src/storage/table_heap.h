#pragma once
#include <string>
#include <utility>

#include "common/config.h"
#include "common/rid.h"

namespace minidb {

class BufferPool;
class LogManager;
class Transaction;

// A heap file: a singly-linked chain of slotted pages. Tuples are opaque byte
// strings (serialized by the record layer). When a LogManager + Transaction are
// supplied, inserts/deletes emit WAL records and stamp page LSNs.
class TableHeap {
 public:
  TableHeap(BufferPool *bpm, page_id_t first_page_id, oid_t oid, LogManager *log = nullptr)
      : bpm_(bpm), first_page_id_(first_page_id), oid_(oid), log_(log) {}

  // Create a brand-new heap (allocates its first page). Returns the first page id.
  static page_id_t Create(BufferPool *bpm);

  page_id_t first_page_id() const { return first_page_id_; }

  bool InsertTuple(const std::string &tuple, RID *out, Transaction *txn);
  bool GetTuple(const RID &rid, std::string *out) const;
  bool DeleteTuple(const RID &rid, Transaction *txn);

  // ---- recovery (no logging; physical apply at exact slot) ----
  void RedoInsert(const RID &rid, const std::string &tuple, lsn_t lsn);
  void RedoDelete(const RID &rid, lsn_t lsn);
  // Re-init `new_page` and relink prev_page->new_page (rebuilds the chain).
  void RedoNewPage(page_id_t prev_page, page_id_t new_page, lsn_t lsn);
  void RestoreTuple(const RID &rid, const std::string &tuple);  // undo of delete
  void RemoveTuple(const RID &rid);                             // undo of insert

  // Forward iterator over live tuples (used by sequential scan).
  class Iterator {
   public:
    Iterator(const TableHeap *heap, RID rid) : heap_(heap), rid_(rid) {}
    bool operator!=(const Iterator &o) const { return !(rid_ == o.rid_); }
    Iterator &operator++() { Advance(); return *this; }
    std::pair<RID, std::string> operator*() const;

   private:
    void Advance();
    const TableHeap *heap_;
    RID rid_;
    friend class TableHeap;
  };

  Iterator Begin() const;
  Iterator End() const { return Iterator(this, RID(INVALID_PAGE_ID, -1)); }

 private:
  BufferPool *bpm_;
  page_id_t first_page_id_;
  oid_t oid_;
  LogManager *log_;
};

}  // namespace minidb
