#include <cstdio>
#include <memory>

#include "engine/database.h"
#include "test_harness.h"

using namespace minidb;

static void Clean(const std::string &base) {
  std::remove((base + ".db").c_str());
  std::remove((base + ".wal").c_str());
  std::remove((base + ".meta").c_str());
}

static int CountRows(Database &db, const std::string &table) {
  auto r = db.Execute("SELECT COUNT(*) FROM " + table);
  return r.rows.empty() ? -1 : std::stoi(r.rows[0][0]);
}

TEST(recovery, committed_data_survives_crash_via_wal) {
  Clean("build/_rec1");
  {
    Database db("build/_rec1");
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    for (int i = 0; i < 300; i++)  // enough rows to span multiple heap pages
      db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 2) + ")");
    db.SimulateCrash();  // drop everything WITHOUT flushing dirty pages
  }
  // Reopen: recovery must redo committed inserts purely from the WAL.
  Database db2("build/_rec1");
  REQUIRE_EQ(CountRows(db2, "t"), 300);
  auto r = db2.Execute("SELECT v FROM t WHERE id = 250");  // also checks rebuilt PK index
  REQUIRE_EQ(r.rows[0][0], std::string("500"));
}

TEST(recovery, uncommitted_transaction_is_rolled_back) {
  Clean("build/_rec2");
  {
    Database db("build/_rec2");
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    for (int i = 0; i < 10; i++)  // committed (autocommit)
      db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");

    db.Execute("BEGIN");
    for (int i = 100; i < 130; i++)  // uncommitted
      db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    // no COMMIT
    db.SimulateCrash();
  }
  Database db2("build/_rec2");
  REQUIRE_EQ(CountRows(db2, "t"), 10);                              // only committed rows
  REQUIRE_EQ(static_cast<int>(db2.Execute("SELECT v FROM t WHERE id = 105").rows.size()), 0);
  REQUIRE_EQ(db2.Execute("SELECT v FROM t WHERE id = 5").rows[0][0], std::string("5"));
}

TEST(recovery, explicit_commit_then_crash) {
  Clean("build/_rec3");
  {
    Database db("build/_rec3");
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    db.Execute("BEGIN");
    for (int i = 0; i < 25; i++)
      db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    db.Execute("COMMIT");  // durable
    db.SimulateCrash();
  }
  Database db2("build/_rec3");
  REQUIRE_EQ(CountRows(db2, "t"), 25);
}

TEST(recovery, clean_reopen_without_crash) {
  Clean("build/_rec4");
  {
    Database db("build/_rec4");
    db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    for (int i = 0; i < 40; i++) db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    // normal destructor: flush pages + persist catalog
  }
  Database db2("build/_rec4");
  REQUIRE_EQ(CountRows(db2, "t"), 40);
}
