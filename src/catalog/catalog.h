#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catalog/schema.h"
#include "common/config.h"
#include "index/bplus_tree.h"

namespace minidb {

class BufferPool;
class LogManager;
class TableHeap;

// Per-column statistics used by the cost-based optimizer.
struct ColumnStats {
  uint64_t ndv{1};      // number of distinct values
  Value min_val;
  Value max_val;
};

struct TableStats {
  uint64_t num_rows{0};
  std::vector<ColumnStats> columns;  // parallel to schema
};

// An index over a single column. Owns its B+Tree handle (root tracked in memory;
// rebuilt from the heap on startup).
struct IndexInfo {
  std::string name;
  int key_col{-1};
  bool primary{false};
  std::unique_ptr<BPlusTree> tree;
};

// All metadata for one table: schema, its heap, indexes, and cached stats.
struct TableInfo {
  std::string name;
  oid_t oid{0};
  Schema schema;
  page_id_t first_page_id{INVALID_PAGE_ID};
  std::unique_ptr<TableHeap> heap;
  std::vector<std::unique_ptr<IndexInfo>> indexes;
  TableStats stats;
  bool stats_dirty{true};
};

// Owns all table metadata and persists table/index definitions to a side file
// so schemas survive restarts. Index B+Trees are rebuilt from heap data on load.
class Catalog {
 public:
  Catalog(BufferPool *bpm, LogManager *log, const std::string &meta_file);

  TableInfo *CreateTable(const std::string &name, const Schema &schema);
  TableInfo *GetTable(const std::string &name);
  TableInfo *GetTableByOid(oid_t oid);
  std::vector<std::string> TableNames() const;

  // Drop and rebuild every index of a table from current heap contents
  // (used after a transaction rollback, since indexes are not WAL-logged).
  void RebuildAllIndexes(TableInfo *ti);

  // Build a B+Tree index over table.col. Populates it from existing rows.
  IndexInfo *CreateIndex(const std::string &index_name, const std::string &table,
                         const std::string &col, bool primary);
  // First index whose key column == col, or nullptr.
  IndexInfo *GetIndexOnColumn(const std::string &table, int col);

  // Stats, recomputed by a heap scan when marked dirty.
  const TableStats &GetStats(const std::string &table);
  void MarkStatsDirty(const std::string &table);

  void Persist();   // write table/index defs to the meta file
  void Load();      // read defs and rebuild heaps + indexes

 private:
  void Analyze(TableInfo *ti);
  void RebuildIndex(TableInfo *ti, IndexInfo *idx);

  BufferPool *bpm_;
  LogManager *log_;
  std::string meta_file_;
  std::map<std::string, std::unique_ptr<TableInfo>> tables_;
  oid_t next_oid_{1};
};

}  // namespace minidb
