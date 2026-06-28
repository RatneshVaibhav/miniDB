#include "optimizer/optimizer.h"

#include <cstring>

#include "common/exception.h"

namespace minidb {

static IndexKey MinKey() { IndexKey k; std::memset(k.data, 0x00, IndexKey::LEN); return k; }
static IndexKey MaxKey() { IndexKey k; std::memset(k.data, 0xff, IndexKey::LEN); return k; }

static std::string Indent(int n) { return std::string(n * 2, ' '); }

// Default selectivity guesses when stats are unavailable.
static constexpr double kRangeSelectivity = 0.3;

double Optimizer::EstimateRows(TableInfo *ti, const std::vector<Predicate> &preds) {
  const TableStats &st = ctx_->catalog->GetStats(ti->name);
  double rows = static_cast<double>(std::max<uint64_t>(1, st.num_rows));
  for (auto &p : preds) {
    if (p.rhs_is_col) continue;
    int col = ti->schema.IndexOf(p.col);
    if (col < 0) continue;
    double ndv = static_cast<double>(std::max<uint64_t>(1, st.columns[col].ndv));
    if (p.op == CompOp::EQ) rows /= ndv;                 // equality -> 1/NDV
    else if (p.op != CompOp::NE) rows *= kRangeSelectivity;  // range
  }
  return std::max(1.0, rows);
}

// Pick a usable predicate whose column is indexed; prefer equality.
static const Predicate *PickIndexablePred(TableInfo *ti, const std::vector<Predicate> &preds,
                                          IndexInfo **out_idx) {
  const Predicate *eq = nullptr;
  const Predicate *rng = nullptr;
  IndexInfo *eq_idx = nullptr, *rng_idx = nullptr;
  for (auto &p : preds) {
    if (p.rhs_is_col) continue;
    int col = ti->schema.IndexOf(p.col);
    if (col < 0) continue;
    IndexInfo *idx = nullptr;
    for (auto &ix : ti->indexes) if (ix->key_col == col) { idx = ix.get(); break; }
    if (!idx) continue;
    if (p.op == CompOp::EQ && !eq) { eq = &p; eq_idx = idx; }
    else if ((p.op == CompOp::LT || p.op == CompOp::LE || p.op == CompOp::GT || p.op == CompOp::GE) && !rng) {
      rng = &p; rng_idx = idx;
    }
  }
  if (eq) { *out_idx = eq_idx; return eq; }
  if (rng) { *out_idx = rng_idx; return rng; }
  return nullptr;
}

std::unique_ptr<Executor> Optimizer::BuildScan(TableInfo *ti, const std::vector<Predicate> &preds,
                                               std::string *explain, int indent) {
  const TableStats &st = ctx_->catalog->GetStats(ti->name);
  double seq_cost = static_cast<double>(std::max<uint64_t>(1, st.num_rows));

  IndexInfo *idx = nullptr;
  const Predicate *p = PickIndexablePred(ti, preds, &idx);
  if (p) {
    double index_cost = 1.0 + EstimateRows(ti, {*p});  // probe + matched rows
    if (index_cost < seq_cost) {
      IndexKey low = MinKey(), high = MaxKey();
      switch (p->op) {
        case CompOp::EQ: low = high = IndexKey::FromValue(p->rhs_val); break;
        case CompOp::LT: case CompOp::LE: high = IndexKey::FromValue(p->rhs_val); break;
        case CompOp::GT: case CompOp::GE: low = IndexKey::FromValue(p->rhs_val); break;
        default: break;
      }
      *explain += Indent(indent) + "IndexScan(" + ti->name + " using " + idx->name +
                  " on " + p->col + " " + CompOpStr(p->op) + " " + p->rhs_val.ToString() +
                  ")  est_rows=" + std::to_string(static_cast<long>(index_cost)) +
                  " [index_cost=" + std::to_string(static_cast<long>(index_cost)) +
                  " < seq_cost=" + std::to_string(static_cast<long>(seq_cost)) + "]\n";
      // residual = all single-table predicates, re-checked for correctness
      return std::make_unique<IndexScanExecutor>(ctx_, ti, idx, low, high, preds);
    }
  }
  *explain += Indent(indent) + "SeqScan(" + ti->name + ")  est_rows=" +
              std::to_string(static_cast<long>(EstimateRows(ti, preds))) +
              " [seq_cost=" + std::to_string(static_cast<long>(seq_cost)) + "]\n";
  return std::make_unique<SeqScanExecutor>(ctx_, ti, preds);
}

CompiledPlan Optimizer::Plan(const Statement &stmt) {
  switch (stmt.type) {
    case StmtType::SELECT: return PlanSelect(stmt);
    case StmtType::INSERT: return PlanInsert(stmt);
    case StmtType::DELETE: return PlanDelete(stmt);
    default: throw DBException("optimizer: unsupported statement");
  }
}

CompiledPlan Optimizer::PlanSelect(const Statement &stmt) {
  CompiledPlan plan;
  TableInfo *t1 = ctx_->catalog->GetTable(stmt.table);
  if (!t1) throw DBException("no such table: " + stmt.table);

  std::unique_ptr<Executor> source;
  Schema source_schema;

  if (!stmt.join.present) {
    source = BuildScan(t1, stmt.where, &plan.explain, 0);
    source_schema = t1->schema;
  } else {
    TableInfo *t2 = ctx_->catalog->GetTable(stmt.join.table);
    if (!t2) throw DBException("no such table: " + stmt.join.table);

    // Resolve which join column belongs to which table.
    std::string a = stmt.join.left_col, b = stmt.join.right_col;
    std::string t1col, t2col;
    if (t1->schema.IndexOf(a) >= 0 && t2->schema.IndexOf(b) >= 0) { t1col = a; t2col = b; }
    else if (t1->schema.IndexOf(b) >= 0 && t2->schema.IndexOf(a) >= 0) { t1col = b; t2col = a; }
    else throw DBException("join columns not found on the two tables");

    // Split single-table predicates; cross-table predicates filter post-join.
    std::vector<Predicate> p1, p2, post;
    for (auto &p : stmt.where) {
      bool in1 = t1->schema.IndexOf(p.col) >= 0;
      bool in2 = t2->schema.IndexOf(p.col) >= 0;
      if (!p.rhs_is_col && in1 && !in2) p1.push_back(p);
      else if (!p.rhs_is_col && in2 && !in1) p2.push_back(p);
      else post.push_back(p);
    }

    // Join-order decision: outer rows * inner access cost, picking the cheaper.
    const TableStats &s1 = ctx_->catalog->GetStats(t1->name);
    const TableStats &s2 = ctx_->catalog->GetStats(t2->name);
    auto inner_cost = [&](TableInfo *ti, const std::string &joincol) {
      int col = ti->schema.IndexOf(joincol);
      bool has_idx = false;
      for (auto &ix : ti->indexes) if (ix->key_col == col) has_idx = true;
      const TableStats &st = ctx_->catalog->GetStats(ti->name);
      double rows = static_cast<double>(std::max<uint64_t>(1, st.num_rows));
      return has_idx ? (1.0 + rows / std::max<uint64_t>(1, st.columns[col].ndv)) : rows;
    };
    double costA = std::max<uint64_t>(1, s1.num_rows) * inner_cost(t2, t2col);  // t1 outer
    double costB = std::max<uint64_t>(1, s2.num_rows) * inner_cost(t1, t1col);  // t2 outer

    TableInfo *outer, *inner;
    std::string outer_col, inner_col;
    std::vector<Predicate> outer_p, inner_p;
    if (costA <= costB) { outer = t1; inner = t2; outer_col = t1col; inner_col = t2col; outer_p = p1; inner_p = p2; }
    else                { outer = t2; inner = t1; outer_col = t2col; inner_col = t1col; outer_p = p2; inner_p = p1; }

    plan.explain += "NestedLoopJoin(outer=" + outer->name + ", inner=" + inner->name +
                    " on " + outer_col + "=" + inner_col + ")  [costA(" + t1->name + " outer)=" +
                    std::to_string(static_cast<long>(costA)) + " vs costB(" + t2->name + " outer)=" +
                    std::to_string(static_cast<long>(costB)) + "]\n";

    auto outer_scan = BuildScan(outer, outer_p, &plan.explain, 1);
    int inner_key_col = inner->schema.IndexOf(inner_col);

    // Combined schema: outer columns then inner columns.
    std::vector<Column> cols = outer->schema.columns();
    for (auto &c : inner->schema.columns()) cols.push_back(c);
    source_schema = Schema(cols);

    // Probe the inner per outer row; the factory enables index-NLJ when possible.
    ExecutorContext *ctx = ctx_;
    std::string inner_explain_hdr;
    {
      std::string tmp;
      // record inner access path once for EXPLAIN (probe with a dummy key)
      (void)tmp;
    }
    IndexInfo *inner_idx = nullptr;
    for (auto &ix : inner->indexes) if (ix->key_col == inner_key_col) inner_idx = ix.get();
    if (inner_idx) plan.explain += Indent(1) + "IndexNLJ inner: IndexScan(" + inner->name +
                                   " using " + inner_idx->name + ")\n";
    else plan.explain += Indent(1) + "inner: SeqScan(" + inner->name + ")\n";

    std::vector<Predicate> inner_residual = inner_p;
    auto factory = [ctx, inner, inner_idx, inner_key_col, inner_residual](const Value &key) -> std::unique_ptr<Executor> {
      if (inner_idx) {
        IndexKey k = IndexKey::FromValue(key);
        return std::make_unique<IndexScanExecutor>(ctx, inner, inner_idx, k, k, inner_residual);
      }
      return std::make_unique<SeqScanExecutor>(ctx, inner, inner_residual);
    };

    int outer_key_col = outer->schema.IndexOf(outer_col);
    source = std::make_unique<NestedLoopJoinExecutor>(std::move(outer_scan), outer_key_col,
                                                      inner_key_col, factory, source_schema);
    if (!post.empty()) {
      plan.explain += "Filter(post-join predicates)\n";
      source = std::make_unique<FilterExecutor>(std::move(source), post);
    }
  }

  // Top: aggregate if any aggregate/group-by, else projection.
  bool has_agg = !stmt.group_by.empty();
  for (auto &it : stmt.select_items) if (it.agg != AggType::NONE) has_agg = true;

  std::unique_ptr<Executor> top;
  if (has_agg) {
    plan.explain += "Aggregate(" + std::to_string(stmt.select_items.size()) + " items)\n";
    top = std::make_unique<AggregateExecutor>(std::move(source), stmt.select_items, stmt.group_by);
  } else {
    plan.explain += "Projection\n";
    top = std::make_unique<ProjectionExecutor>(std::move(source), stmt.select_items);
  }
  for (auto &c : top->GetOutputSchema().columns()) plan.column_names.push_back(c.name);
  plan.root = std::move(top);
  return plan;
}

CompiledPlan Optimizer::PlanInsert(const Statement &stmt) {
  CompiledPlan plan;
  plan.is_dml = true;
  TableInfo *ti = ctx_->catalog->GetTable(stmt.table);
  if (!ti) throw DBException("no such table: " + stmt.table);
  const Schema &sc = ti->schema;

  std::vector<Tuple> tuples;
  for (auto &raw : stmt.rows) {
    std::vector<Value> vals(sc.column_count(), Value::MakeNull(TypeId::INVALID));
    if (stmt.insert_cols.empty()) {
      if (raw.size() != sc.column_count()) throw DBException("INSERT: column count mismatch");
      for (size_t i = 0; i < raw.size(); i++) vals[i] = raw[i];
    } else {
      if (raw.size() != stmt.insert_cols.size()) throw DBException("INSERT: value/column count mismatch");
      for (size_t i = 0; i < stmt.insert_cols.size(); i++) {
        int ci = sc.IndexOf(stmt.insert_cols[i]);
        if (ci < 0) throw DBException("INSERT: no such column " + stmt.insert_cols[i]);
        vals[ci] = raw[i];
      }
    }
    tuples.emplace_back(std::move(vals));
  }
  plan.explain = "Insert(" + stmt.table + ", " + std::to_string(tuples.size()) + " rows)\n";
  plan.root = std::make_unique<InsertExecutor>(ctx_, ti, std::move(tuples));
  return plan;
}

CompiledPlan Optimizer::PlanDelete(const Statement &stmt) {
  CompiledPlan plan;
  plan.is_dml = true;
  TableInfo *ti = ctx_->catalog->GetTable(stmt.table);
  if (!ti) throw DBException("no such table: " + stmt.table);
  auto scan = BuildScan(ti, stmt.where, &plan.explain, 1);
  plan.explain = "Delete(" + stmt.table + ")\n" + plan.explain;
  plan.root = std::make_unique<DeleteExecutor>(ctx_, ti, std::move(scan));
  return plan;
}

}  // namespace minidb
