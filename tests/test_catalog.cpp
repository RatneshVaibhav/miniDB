#include "catalog/catalog.h"
#include "record/tuple.h"
#include "recovery/log_manager.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/table_heap.h"
#include "test_harness.h"

using namespace minidb;

static Schema MakeUserSchema() {
  std::vector<Column> cols = {
      {"id", TypeId::INTEGER, 0},
      {"name", TypeId::VARCHAR, 32},
      {"age", TypeId::INTEGER, 0},
  };
  return Schema(cols, 0);  // pk = id
}

TEST(catalog, create_insert_index_search) {
  std::remove("build/_cat.db");
  std::remove("build/_cat.meta");
  DiskManager dm("build/_cat.db");
  LogManager lm("build/_cat.wal");
  BufferPool bp(64, &dm);
  Catalog cat(&bp, &lm, "build/_cat.meta");

  TableInfo *ti = cat.CreateTable("users", MakeUserSchema());
  REQUIRE(ti != nullptr);
  // primary-key index auto-created
  REQUIRE_EQ(static_cast<int>(ti->indexes.size()), 1);

  for (int i = 0; i < 200; i++) {
    Tuple row({Value::Int(i), Value::Varchar("user" + std::to_string(i)), Value::Int(20 + i % 50)});
    RID rid;
    ti->heap->InsertTuple(row.Serialize(ti->schema), &rid, nullptr);
    ti->indexes[0]->tree->Insert(IndexKey::FromValue(Value::Int(i)), rid);
  }
  cat.MarkStatsDirty("users");

  RID found;
  REQUIRE(ti->indexes[0]->tree->Search(IndexKey::FromValue(Value::Int(150)), &found));
  std::string bytes;
  REQUIRE(ti->heap->GetTuple(found, &bytes));
  Tuple t = Tuple::Deserialize(ti->schema, bytes);
  REQUIRE_EQ(t.value(0).as_int(), 150);
  REQUIRE_EQ(t.value(1).as_string(), std::string("user150"));

  const TableStats &st = cat.GetStats("users");
  REQUIRE_EQ(static_cast<int>(st.num_rows), 200);
  REQUIRE_EQ(static_cast<int>(st.columns[0].ndv), 200);  // id is unique
}

TEST(catalog, persist_and_reload_rebuilds_index) {
  std::remove("build/_cat2.db");
  std::remove("build/_cat2.meta");
  std::remove("build/_cat2.wal");
  {
    DiskManager dm("build/_cat2.db");
    LogManager lm("build/_cat2.wal");
    BufferPool bp(64, &dm);
    Catalog cat(&bp, &lm, "build/_cat2.meta");
    TableInfo *ti = cat.CreateTable("users", MakeUserSchema());
    for (int i = 0; i < 50; i++) {
      Tuple row({Value::Int(i), Value::Varchar("u" + std::to_string(i)), Value::Int(30)});
      RID rid;
      ti->heap->InsertTuple(row.Serialize(ti->schema), &rid, nullptr);
      ti->indexes[0]->tree->Insert(IndexKey::FromValue(Value::Int(i)), rid);
    }
    bp.FlushAll();
  }
  // Reopen: catalog reloads schema and rebuilds the index from the heap.
  DiskManager dm("build/_cat2.db");
  LogManager lm("build/_cat2.wal");
  BufferPool bp(64, &dm);
  Catalog cat(&bp, &lm, "build/_cat2.meta");
  cat.Load();
  TableInfo *ti = cat.GetTable("users");
  REQUIRE(ti != nullptr);
  REQUIRE_EQ(static_cast<int>(ti->indexes.size()), 1);
  RID r;
  REQUIRE(ti->indexes[0]->tree->Search(IndexKey::FromValue(Value::Int(25)), &r));
  std::string bytes;
  REQUIRE(ti->heap->GetTuple(r, &bytes));
  REQUIRE_EQ(Tuple::Deserialize(ti->schema, bytes).value(1).as_string(), std::string("u25"));
}
