#pragma once
#include <memory>
#include <string>
#include <vector>

#include "execution/executor.h"
#include "sql/ast.h"

namespace minidb {

// A planned, ready-to-run query: the operator tree, output column labels, a
// human-readable EXPLAIN string, and whether it is a DML (INSERT/DELETE) plan.
struct CompiledPlan {
  std::unique_ptr<Executor> root;
  std::vector<std::string> column_names;
  std::string explain;
  bool is_dml{false};
};

// Cost-based optimizer. Chooses between sequential and index scans using
// selectivity estimates, and picks a join order for two-table joins.
class Optimizer {
 public:
  explicit Optimizer(ExecutorContext *ctx) : ctx_(ctx) {}

  CompiledPlan Plan(const Statement &stmt);

 private:
  CompiledPlan PlanSelect(const Statement &stmt);
  CompiledPlan PlanInsert(const Statement &stmt);
  CompiledPlan PlanDelete(const Statement &stmt);

  // Build the best single-table access path for `preds`. Appends a description
  // to `explain`. `out_uses_index` reports the decision (for join costing).
  std::unique_ptr<Executor> BuildScan(TableInfo *ti, const std::vector<Predicate> &preds,
                                      std::string *explain, int indent);

  // Estimated rows returned by scanning `ti` under `preds`.
  double EstimateRows(TableInfo *ti, const std::vector<Predicate> &preds);

  ExecutorContext *ctx_;
};

}  // namespace minidb
