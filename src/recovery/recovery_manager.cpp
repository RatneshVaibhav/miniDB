#include "recovery/recovery_manager.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalog/catalog.h"
#include "recovery/log_manager.h"
#include "storage/buffer_pool.h"
#include "storage/table_heap.h"

namespace minidb {

bool RecoveryManager::Recover() {
  std::vector<LogRecord> log = log_->ReadAll();
  if (log.empty()) return false;

  // ---- Analysis: which transactions committed? ----
  std::unordered_set<txn_id_t> committed;
  std::unordered_set<txn_id_t> seen;
  for (auto &r : log) {
    seen.insert(r.txn_id);
    if (r.type == LogType::COMMIT) committed.insert(r.txn_id);
  }

  auto heap_for = [&](oid_t oid) -> TableHeap * {
    TableInfo *ti = catalog_->GetTableByOid(oid);
    return ti ? ti->heap.get() : nullptr;
  };

  // ---- Redo: repeat history for all data/structural records ----
  for (auto &r : log) {
    TableHeap *h = heap_for(r.table_oid);
    if (!h) continue;
    switch (r.type) {
      case LogType::NEWPAGE: h->RedoNewPage(r.rid.slot, r.rid.page_id, r.lsn); break;
      case LogType::INSERT:  h->RedoInsert(r.rid, r.new_tuple, r.lsn); break;
      case LogType::MARK_DELETE: h->RedoDelete(r.rid, r.lsn); break;
      default: break;
    }
  }

  // ---- Undo: roll back losers in reverse order ----
  std::unordered_set<txn_id_t> touched_tables_seen;
  std::unordered_set<oid_t> touched_oids;
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    LogRecord &r = *it;
    if (committed.count(r.txn_id)) continue;  // winner: keep
    TableHeap *h = heap_for(r.table_oid);
    if (!h) continue;
    if (r.type == LogType::INSERT) { h->RemoveTuple(r.rid); touched_oids.insert(r.table_oid); }
    else if (r.type == LogType::MARK_DELETE) { h->RestoreTuple(r.rid, r.old_tuple); touched_oids.insert(r.table_oid); }
  }

  // Redo may have re-added committed inserts; collect their tables too so all
  // indexes are rebuilt consistently from the final heap state.
  for (auto &r : log) {
    if (r.type == LogType::INSERT || r.type == LogType::MARK_DELETE) touched_oids.insert(r.table_oid);
  }

  for (oid_t oid : touched_oids) {
    TableInfo *ti = catalog_->GetTableByOid(oid);
    if (ti) catalog_->RebuildAllIndexes(ti);
  }

  bpm_->FlushAll();  // make the recovered state durable
  return true;
}

}  // namespace minidb
