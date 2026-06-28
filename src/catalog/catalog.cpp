#include "catalog/catalog.h"

#include <fstream>
#include <set>
#include <sstream>

#include "common/exception.h"
#include "record/tuple.h"
#include "storage/table_heap.h"

namespace minidb {

Catalog::Catalog(BufferPool *bpm, LogManager *log, const std::string &meta_file)
    : bpm_(bpm), log_(log), meta_file_(meta_file) {}

TableInfo *Catalog::CreateTable(const std::string &name, const Schema &schema) {
  if (tables_.count(name)) throw DBException("table already exists: " + name);
  auto ti = std::make_unique<TableInfo>();
  ti->name = name;
  ti->oid = next_oid_++;
  ti->schema = schema;
  ti->first_page_id = TableHeap::Create(bpm_);
  ti->heap = std::make_unique<TableHeap>(bpm_, ti->first_page_id, ti->oid, log_);
  TableInfo *raw = ti.get();
  tables_[name] = std::move(ti);

  // Auto-create the primary key index if the schema declares one.
  if (schema.pk_index() >= 0) {
    CreateIndex(name + "_pk", name, schema.column(schema.pk_index()).name, true);
  }
  Persist();
  return raw;
}

TableInfo *Catalog::GetTable(const std::string &name) {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : it->second.get();
}

std::vector<std::string> Catalog::TableNames() const {
  std::vector<std::string> out;
  for (auto &[k, v] : tables_) out.push_back(k);
  return out;
}

IndexInfo *Catalog::CreateIndex(const std::string &index_name, const std::string &table,
                                const std::string &col, bool primary) {
  TableInfo *ti = GetTable(table);
  if (!ti) throw DBException("no such table: " + table);
  int kc = ti->schema.IndexOf(col);
  if (kc < 0) throw DBException("no such column: " + col);

  auto idx = std::make_unique<IndexInfo>();
  idx->name = index_name;
  idx->key_col = kc;
  idx->primary = primary;
  idx->tree = std::make_unique<BPlusTree>(bpm_, primary);
  IndexInfo *raw = idx.get();
  ti->indexes.push_back(std::move(idx));
  RebuildIndex(ti, raw);
  Persist();
  return raw;
}

TableInfo *Catalog::GetTableByOid(oid_t oid) {
  for (auto &[name, ti] : tables_) {
    if (ti->oid == oid) return ti.get();
  }
  return nullptr;
}

void Catalog::RebuildAllIndexes(TableInfo *ti) {
  for (auto &idx : ti->indexes) {
    idx->tree = std::make_unique<BPlusTree>(bpm_, idx->primary);  // fresh tree; old pages leak (documented)
    RebuildIndex(ti, idx.get());
  }
}

IndexInfo *Catalog::GetIndexOnColumn(const std::string &table, int col) {
  TableInfo *ti = GetTable(table);
  if (!ti) return nullptr;
  for (auto &idx : ti->indexes) {
    if (idx->key_col == col) return idx.get();
  }
  return nullptr;
}

void Catalog::RebuildIndex(TableInfo *ti, IndexInfo *idx) {
  for (auto it = ti->heap->Begin(); it != ti->heap->End(); ++it) {
    auto [rid, bytes] = *it;
    Tuple t = Tuple::Deserialize(ti->schema, bytes);
    idx->tree->Insert(IndexKey::FromValue(t.value(idx->key_col)), rid);
  }
}

void Catalog::MarkStatsDirty(const std::string &table) {
  TableInfo *ti = GetTable(table);
  if (ti) ti->stats_dirty = true;
}

const TableStats &Catalog::GetStats(const std::string &table) {
  TableInfo *ti = GetTable(table);
  if (!ti) throw DBException("no such table: " + table);
  if (ti->stats_dirty) Analyze(ti);
  return ti->stats;
}

void Catalog::Analyze(TableInfo *ti) {
  size_t ncol = ti->schema.column_count();
  TableStats st;
  st.columns.resize(ncol);
  std::vector<std::set<std::string>> distinct(ncol);
  std::vector<Value> mn(ncol), mx(ncol);
  std::vector<bool> seen(ncol, false);

  for (auto it = ti->heap->Begin(); it != ti->heap->End(); ++it) {
    auto [rid, bytes] = *it;
    Tuple t = Tuple::Deserialize(ti->schema, bytes);
    st.num_rows++;
    for (size_t c = 0; c < ncol; c++) {
      const Value &v = t.value(c);
      distinct[c].insert(v.ToString());
      if (!seen[c]) { mn[c] = mx[c] = v; seen[c] = true; }
      else { if (v.Compare(mn[c]) < 0) mn[c] = v; if (v.Compare(mx[c]) > 0) mx[c] = v; }
    }
  }
  for (size_t c = 0; c < ncol; c++) {
    st.columns[c].ndv = std::max<uint64_t>(1, distinct[c].size());
    if (seen[c]) { st.columns[c].min_val = mn[c]; st.columns[c].max_val = mx[c]; }
  }
  ti->stats = std::move(st);
  ti->stats_dirty = false;
}

// ---- persistence: a simple line-based text format ----
void Catalog::Persist() {
  std::ofstream out(meta_file_, std::ios::trunc);
  if (!out.is_open()) return;
  out << next_oid_ << "\n";
  out << tables_.size() << "\n";
  for (auto &[name, ti] : tables_) {
    out << "TABLE " << name << " " << ti->oid << " " << ti->first_page_id << " "
        << ti->schema.column_count() << " " << ti->schema.pk_index() << "\n";
    for (auto &c : ti->schema.columns()) {
      out << "COL " << c.name << " " << static_cast<int>(c.type) << " " << c.length << "\n";
    }
    out << "NINDEX " << ti->indexes.size() << "\n";
    for (auto &idx : ti->indexes) {
      out << "INDEX " << idx->name << " " << idx->key_col << " " << (idx->primary ? 1 : 0) << "\n";
    }
  }
}

void Catalog::Load() {
  std::ifstream in(meta_file_);
  if (!in.is_open()) return;
  in >> next_oid_;
  size_t ntables;
  in >> ntables;
  std::string tag;
  for (size_t t = 0; t < ntables; t++) {
    std::string tname;
    oid_t oid;
    page_id_t first;
    size_t ncol;
    int pk;
    in >> tag >> tname >> oid >> first >> ncol >> pk;  // "TABLE ..."
    std::vector<Column> cols;
    for (size_t c = 0; c < ncol; c++) {
      std::string cname;
      int type;
      uint32_t len;
      in >> tag >> cname >> type >> len;  // "COL ..."
      cols.push_back({cname, static_cast<TypeId>(type), len});
    }
    auto ti = std::make_unique<TableInfo>();
    ti->name = tname;
    ti->oid = oid;
    ti->schema = Schema(cols, pk);
    ti->first_page_id = first;
    ti->heap = std::make_unique<TableHeap>(bpm_, first, oid, log_);

    size_t nindex;
    in >> tag >> nindex;  // "NINDEX n"
    std::vector<std::tuple<std::string, int, bool>> idx_defs;
    for (size_t i = 0; i < nindex; i++) {
      std::string iname;
      int kc, prim;
      in >> tag >> iname >> kc >> prim;  // "INDEX ..."
      idx_defs.emplace_back(iname, kc, prim != 0);
    }
    TableInfo *raw = ti.get();
    tables_[tname] = std::move(ti);
    // Rebuild each index from heap contents.
    for (auto &[iname, kc, prim] : idx_defs) {
      auto idx = std::make_unique<IndexInfo>();
      idx->name = iname;
      idx->key_col = kc;
      idx->primary = prim;
      idx->tree = std::make_unique<BPlusTree>(bpm_, prim);
      IndexInfo *iraw = idx.get();
      raw->indexes.push_back(std::move(idx));
      RebuildIndex(raw, iraw);
    }
  }
}

}  // namespace minidb
