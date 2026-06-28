#include "storage/table_heap.h"

#include "recovery/log_manager.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"
#include "storage/table_page.h"
#include "txn/transaction.h"

namespace minidb {

page_id_t TableHeap::Create(BufferPool *bpm) {
  page_id_t pid;
  Page *page = bpm->NewPage(&pid);
  TablePage tp(page->GetData());
  tp.Init();
  bpm->UnpinPage(pid, true);
  bpm->FlushPage(pid);  // make the head page durable immediately (DDL is rare)
  return pid;
}

bool TableHeap::InsertTuple(const std::string &tuple, RID *out, Transaction *txn) {
  page_id_t cur = first_page_id_;
  while (true) {
    Page *page = bpm_->FetchPage(cur);
    TablePage tp(page->GetData());
    int slot = tp.InsertTuple(tuple.data(), static_cast<uint32_t>(tuple.size()));
    if (slot >= 0) {
      *out = RID(cur, slot);
      // WAL: log the insert, stamp page LSN, extend the txn undo chain.
      if (log_ && txn) {
        LogRecord rec;
        rec.type = LogType::INSERT;
        rec.txn_id = txn->id();
        rec.prev_lsn = txn->prev_lsn();
        rec.table_oid = oid_;
        rec.rid = *out;
        rec.new_tuple = tuple;
        lsn_t lsn = log_->Append(rec);
        txn->set_prev_lsn(lsn);
        page->SetLSN(lsn);
        txn->undo_log().push_back(rec);
      }
      bpm_->UnpinPage(cur, true);
      return true;
    }
    // Page full: advance to the next, allocating one if at the tail.
    page_id_t next = tp.GetNextPageId();
    if (next == INVALID_PAGE_ID) {
      page_id_t newpid;
      Page *np = bpm_->NewPage(&newpid);
      TablePage ntp(np->GetData());
      ntp.Init();
      // WAL: log the structural change so the page chain is crash-durable.
      if (log_ && txn) {
        LogRecord rec;
        rec.type = LogType::NEWPAGE;
        rec.txn_id = txn->id();
        rec.prev_lsn = txn->prev_lsn();
        rec.table_oid = oid_;
        rec.rid = RID(newpid, cur);  // new page id + prev page id (in slot)
        lsn_t lsn = log_->Append(rec);
        txn->set_prev_lsn(lsn);
        np->SetLSN(lsn);
        page->SetLSN(lsn);
        txn->undo_log().push_back(rec);
      }
      bpm_->UnpinPage(newpid, true);
      tp.SetNextPageId(newpid);
      bpm_->UnpinPage(cur, true);
      cur = newpid;
    } else {
      bpm_->UnpinPage(cur, false);
      cur = next;
    }
  }
}

bool TableHeap::GetTuple(const RID &rid, std::string *out) const {
  if (rid.page_id == INVALID_PAGE_ID) return false;
  Page *page = bpm_->FetchPage(rid.page_id);
  TablePage tp(page->GetData());
  bool ok = tp.GetTuple(rid.slot, out);
  bpm_->UnpinPage(rid.page_id, false);
  return ok;
}

bool TableHeap::DeleteTuple(const RID &rid, Transaction *txn) {
  Page *page = bpm_->FetchPage(rid.page_id);
  TablePage tp(page->GetData());
  std::string before;
  if (!tp.GetTuple(rid.slot, &before)) {
    bpm_->UnpinPage(rid.page_id, false);
    return false;
  }
  tp.DeleteTuple(rid.slot);
  if (log_ && txn) {
    LogRecord rec;
    rec.type = LogType::MARK_DELETE;
    rec.txn_id = txn->id();
    rec.prev_lsn = txn->prev_lsn();
    rec.table_oid = oid_;
    rec.rid = rid;
    rec.old_tuple = before;  // before image for undo
    lsn_t lsn = log_->Append(rec);
    txn->set_prev_lsn(lsn);
    page->SetLSN(lsn);
    txn->undo_log().push_back(rec);
  }
  bpm_->UnpinPage(rid.page_id, true);
  return true;
}

// ---- recovery primitives ----
void TableHeap::RedoInsert(const RID &rid, const std::string &tuple, lsn_t lsn) {
  Page *page = bpm_->FetchPage(rid.page_id);
  if (page->GetLSN() < lsn) {
    TablePage tp(page->GetData());
    tp.InitIfNeeded();
    tp.RedoInsert(rid.slot, tuple.data(), static_cast<uint32_t>(tuple.size()));
    page->SetLSN(lsn);
    bpm_->UnpinPage(rid.page_id, true);
  } else {
    bpm_->UnpinPage(rid.page_id, false);
  }
}

void TableHeap::RedoDelete(const RID &rid, lsn_t lsn) {
  Page *page = bpm_->FetchPage(rid.page_id);
  if (page->GetLSN() < lsn) {
    TablePage tp(page->GetData());
    tp.InitIfNeeded();
    tp.DeleteTuple(rid.slot);
    page->SetLSN(lsn);
    bpm_->UnpinPage(rid.page_id, true);
  } else {
    bpm_->UnpinPage(rid.page_id, false);
  }
}

void TableHeap::RedoNewPage(page_id_t prev_page, page_id_t new_page, lsn_t lsn) {
  // Re-initialize the new page if its Init was lost in the crash.
  Page *np = bpm_->FetchPage(new_page);
  if (np->GetLSN() < lsn) {
    TablePage tp(np->GetData());
    tp.InitIfNeeded();
    np->SetLSN(lsn);
    bpm_->UnpinPage(new_page, true);
  } else {
    bpm_->UnpinPage(new_page, false);
  }
  // Restore the chain link prev -> new (idempotent).
  Page *pp = bpm_->FetchPage(prev_page);
  TablePage ptp(pp->GetData());
  ptp.InitIfNeeded();
  ptp.SetNextPageId(new_page);
  bpm_->UnpinPage(prev_page, true);
}

void TableHeap::RestoreTuple(const RID &rid, const std::string &tuple) {
  Page *page = bpm_->FetchPage(rid.page_id);
  TablePage tp(page->GetData());
  tp.RestoreTuple(rid.slot, tuple.data(), static_cast<uint32_t>(tuple.size()));
  bpm_->UnpinPage(rid.page_id, true);
}

void TableHeap::RemoveTuple(const RID &rid) {
  Page *page = bpm_->FetchPage(rid.page_id);
  TablePage tp(page->GetData());
  tp.DeleteTuple(rid.slot);
  bpm_->UnpinPage(rid.page_id, true);
}

// ---- iterator ----
TableHeap::Iterator TableHeap::Begin() const {
  // Find the first live tuple by scanning pages from the head.
  page_id_t cur = first_page_id_;
  while (cur != INVALID_PAGE_ID) {
    Page *page = bpm_->FetchPage(cur);
    TablePage tp(page->GetData());
    uint32_t n = tp.GetNumSlots();
    for (uint32_t i = 0; i < n; i++) {
      std::string tmp;
      if (tp.GetTuple(i, &tmp)) {
        page_id_t found = cur;
        bpm_->UnpinPage(cur, false);
        return Iterator(this, RID(found, static_cast<slot_id_t>(i)));
      }
    }
    page_id_t next = tp.GetNextPageId();
    bpm_->UnpinPage(cur, false);
    cur = next;
  }
  return End();
}

std::pair<RID, std::string> TableHeap::Iterator::operator*() const {
  std::string out;
  heap_->GetTuple(rid_, &out);
  return {rid_, out};
}

void TableHeap::Iterator::Advance() {
  page_id_t cur = rid_.page_id;
  slot_id_t slot = rid_.slot + 1;
  while (cur != INVALID_PAGE_ID) {
    Page *page = heap_->bpm_->FetchPage(cur);
    TablePage tp(page->GetData());
    uint32_t n = tp.GetNumSlots();
    for (uint32_t i = slot; i < n; i++) {
      std::string tmp;
      if (tp.GetTuple(i, &tmp)) {
        rid_ = RID(cur, static_cast<slot_id_t>(i));
        heap_->bpm_->UnpinPage(cur, false);
        return;
      }
    }
    page_id_t next = tp.GetNextPageId();
    heap_->bpm_->UnpinPage(cur, false);
    cur = next;
    slot = 0;
  }
  rid_ = RID(INVALID_PAGE_ID, -1);  // end
}

}  // namespace minidb
