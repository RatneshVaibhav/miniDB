#pragma once
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"

namespace minidb {

enum class StmtType {
  CREATE_TABLE, CREATE_INDEX, INSERT, DELETE, SELECT,
  BEGIN, COMMIT, ABORT, EXPLAIN, SHOW_TABLES
};

enum class CompOp { EQ, NE, LT, LE, GT, GE };

inline std::string CompOpStr(CompOp op) {
  switch (op) {
    case CompOp::EQ: return "=";  case CompOp::NE: return "!=";
    case CompOp::LT: return "<";  case CompOp::LE: return "<=";
    case CompOp::GT: return ">";  case CompOp::GE: return ">=";
  }
  return "?";
}

enum class AggType { NONE, COUNT_STAR, COUNT, SUM, MIN, MAX, AVG };

struct ColumnDef {
  std::string name;
  TypeId type;
  uint32_t length{0};
  bool is_pk{false};
};

// A single WHERE comparison: lhs column `col` OP (literal `rhs_val` | column `rhs_col`).
struct Predicate {
  std::string col;
  CompOp op{CompOp::EQ};
  bool rhs_is_col{false};
  Value rhs_val;
  std::string rhs_col;
};

struct SelectItem {
  bool is_star{false};
  AggType agg{AggType::NONE};
  std::string col;     // column name (plain col, or aggregate argument)
  std::string alias;   // output label
};

struct JoinClause {
  bool present{false};
  std::string table;       // right-hand table
  std::string left_col;    // equi-join key on the left table
  std::string right_col;   // equi-join key on the right table
};

// One parsed SQL statement. A single struct covers all statement kinds; only the
// fields relevant to `type` are populated.
struct Statement {
  StmtType type;

  std::string table;                 // primary/target table
  std::vector<ColumnDef> columns;    // CREATE TABLE

  std::string index_name;            // CREATE INDEX
  std::string index_col;

  std::vector<std::string> insert_cols;        // INSERT (optional explicit cols)
  std::vector<std::vector<Value>> rows;        // INSERT values

  std::vector<SelectItem> select_items;        // SELECT projection
  JoinClause join;                             // SELECT join
  std::vector<Predicate> where;                // ANDed predicates (SELECT/DELETE)
  std::vector<std::string> group_by;           // SELECT group by

  std::shared_ptr<Statement> inner;            // EXPLAIN <stmt>
};

}  // namespace minidb
