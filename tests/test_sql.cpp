#include <cstdio>

#include "engine/database.h"
#include "test_harness.h"

using namespace minidb;

static void Clean(const std::string &base) {
  std::remove((base + ".db").c_str());
  std::remove((base + ".wal").c_str());
  std::remove((base + ".meta").c_str());
}

TEST(sql, create_insert_select_where) {
  Clean("build/_sql1");
  Database db("build/_sql1");
  REQUIRE(db.Execute("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(32), age INT)").ok);
  for (int i = 0; i < 20; i++) {
    auto r = db.Execute("INSERT INTO users VALUES (" + std::to_string(i) + ", 'u" + std::to_string(i) +
                        "', " + std::to_string(20 + i) + ")");
    REQUIRE(r.ok);
    REQUIRE_EQ(static_cast<int>(r.affected), 1);
  }
  auto r = db.Execute("SELECT id, name FROM users WHERE age >= 35");
  REQUIRE(r.ok);
  REQUIRE_EQ(static_cast<int>(r.rows.size()), 5);  // ages 35..39 -> ids 15..19
  REQUIRE_EQ(r.columns.size(), 2u);
}

TEST(sql, primary_key_index_point_lookup) {
  Clean("build/_sql2");
  Database db("build/_sql2");
  db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
  for (int i = 0; i < 100; i++)
    db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");

  auto r = db.Execute("SELECT v FROM t WHERE id = 42");
  REQUIRE(r.ok);
  REQUIRE_EQ(static_cast<int>(r.rows.size()), 1);
  REQUIRE_EQ(r.rows[0][0], std::string("420"));

  // EXPLAIN should choose an index scan for the selective equality on the PK.
  auto e = db.Execute("EXPLAIN SELECT v FROM t WHERE id = 42");
  REQUIRE(e.ok);
  REQUIRE(e.explain.find("IndexScan") != std::string::npos);
}

TEST(sql, optimizer_prefers_seqscan_for_unindexed) {
  Clean("build/_sql3");
  Database db("build/_sql3");
  db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
  for (int i = 0; i < 100; i++)
    db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 5) + ")");
  // filter on non-indexed column v -> seq scan
  auto e = db.Execute("EXPLAIN SELECT id FROM t WHERE v = 3");
  REQUIRE(e.explain.find("SeqScan") != std::string::npos);
}

TEST(sql, join_two_tables) {
  Clean("build/_sql4");
  Database db("build/_sql4");
  db.Execute("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(16))");
  db.Execute("CREATE TABLE orders (oid INT PRIMARY KEY, uid INT, amount INT)");
  db.Execute("INSERT INTO users VALUES (1,'alice'),(2,'bob'),(3,'carol')");
  db.Execute("INSERT INTO orders VALUES (10,1,100),(11,1,200),(12,2,50)");

  auto r = db.Execute("SELECT name, amount FROM users JOIN orders ON id = uid");
  REQUIRE(r.ok);
  REQUIRE_EQ(static_cast<int>(r.rows.size()), 3);  // alice x2, bob x1
}

TEST(sql, delete_rows) {
  Clean("build/_sql5");
  Database db("build/_sql5");
  db.Execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
  for (int i = 0; i < 50; i++) db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
  auto d = db.Execute("DELETE FROM t WHERE id < 10");
  REQUIRE(d.ok);
  REQUIRE_EQ(static_cast<int>(d.affected), 10);
  auto c = db.Execute("SELECT COUNT(*) FROM t");
  REQUIRE_EQ(c.rows[0][0], std::string("40"));
  // deleted key gone from index
  auto g = db.Execute("SELECT v FROM t WHERE id = 5");
  REQUIRE_EQ(static_cast<int>(g.rows.size()), 0);
}

TEST(sql, aggregates_and_group_by) {
  Clean("build/_sql6");
  Database db("build/_sql6");
  db.Execute("CREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(8), amt INT)");
  db.Execute("INSERT INTO sales VALUES (1,'east',10),(2,'east',20),(3,'west',30),(4,'west',40),(5,'west',5)");
  auto r = db.Execute("SELECT region, COUNT(*), SUM(amt) FROM sales GROUP BY region");
  REQUIRE(r.ok);
  REQUIRE_EQ(static_cast<int>(r.rows.size()), 2);
  auto tot = db.Execute("SELECT SUM(amt), MIN(amt), MAX(amt) FROM sales");
  REQUIRE_EQ(tot.rows[0][0], std::string("105"));
  REQUIRE_EQ(tot.rows[0][1], std::string("5"));
  REQUIRE_EQ(tot.rows[0][2], std::string("40"));
}

TEST(sql, secondary_index_non_unique_returns_all_rows) {
  Clean("build/_sql8");
  Database db("build/_sql8");
  db.Execute("CREATE TABLE s (id INT PRIMARY KEY, branch VARCHAR(8))");
  db.Execute("INSERT INTO s VALUES (1,'cse'),(2,'ece'),(3,'cse'),(4,'me'),(5,'cse'),(6,'ece')");
  db.Execute("CREATE INDEX idx_b ON s (branch)");
  // non-unique key: all three 'cse' rows must come back via the index
  auto r = db.Execute("SELECT id FROM s WHERE branch = 'cse'");
  REQUIRE(r.ok);
  REQUIRE_EQ(static_cast<int>(r.rows.size()), 3);
  // deleting one row removes only its index entry
  db.Execute("DELETE FROM s WHERE id = 3");
  auto r2 = db.Execute("SELECT id FROM s WHERE branch = 'cse'");
  REQUIRE_EQ(static_cast<int>(r2.rows.size()), 2);
  // a different value is unaffected
  REQUIRE_EQ(static_cast<int>(db.Execute("SELECT id FROM s WHERE branch = 'ece'").rows.size()), 2);
}

TEST(sql, secondary_index_used) {
  Clean("build/_sql7");
  Database db("build/_sql7");
  db.Execute("CREATE TABLE t (id INT PRIMARY KEY, cat INT, v INT)");
  for (int i = 0; i < 200; i++)
    db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i) + "," + std::to_string(i) + ")");
  db.Execute("CREATE INDEX idx_cat ON t (cat)");
  auto e = db.Execute("EXPLAIN SELECT v FROM t WHERE cat = 77");
  REQUIRE(e.explain.find("IndexScan") != std::string::npos);
  auto r = db.Execute("SELECT v FROM t WHERE cat = 77");
  REQUIRE_EQ(r.rows[0][0], std::string("77"));
}
