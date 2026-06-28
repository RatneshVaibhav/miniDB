#include "execution/executor.h"

#include "common/exception.h"
#include "storage/table_heap.h"
#include "txn/lock_manager.h"
#include "txn/transaction.h"

namespace minidb {

// ---- predicate evaluation ----
static bool ApplyOp(CompOp op, int cmp) {
  switch (op) {
    case CompOp::EQ: return cmp == 0;
    case CompOp::NE: return cmp != 0;
    case CompOp::LT: return cmp < 0;
    case CompOp::LE: return cmp <= 0;
    case CompOp::GT: return cmp > 0;
    case CompOp::GE: return cmp >= 0;
  }
  return false;
}

bool EvalPredicate(const Predicate &p, const Tuple &t, const Schema &schema) {
  int lc = schema.IndexOf(p.col);
  if (lc < 0) return false;
  const Value &lhs = t.value(lc);
  if (p.rhs_is_col) {
    int rc = schema.IndexOf(p.rhs_col);
    if (rc < 0) return false;
    return ApplyOp(p.op, lhs.Compare(t.value(rc)));
  }
  return ApplyOp(p.op, lhs.Compare(p.rhs_val));
}

bool EvalPredicates(const std::vector<Predicate> &ps, const Tuple &t, const Schema &schema) {
  for (auto &p : ps) {
    if (!EvalPredicate(p, t, schema)) return false;
  }
  return true;
}

static void MaybeLockShared(ExecutorContext *ctx, const RID &rid) {
  if (ctx->lock_mgr && ctx->txn) ctx->lock_mgr->LockShared(ctx->txn, rid);
}
static void MaybeLockExclusive(ExecutorContext *ctx, const RID &rid) {
  if (ctx->lock_mgr && ctx->txn) ctx->lock_mgr->LockExclusive(ctx->txn, rid);
}

// ---- SeqScan ----
SeqScanExecutor::SeqScanExecutor(ExecutorContext *ctx, TableInfo *table, std::vector<Predicate> preds)
    : ctx_(ctx), table_(table), preds_(std::move(preds)) {}

void SeqScanExecutor::Init() {
  it_ = std::make_unique<TableHeap::Iterator>(table_->heap->Begin());
  end_ = std::make_unique<TableHeap::Iterator>(table_->heap->End());
}

bool SeqScanExecutor::Next(Tuple *out, RID *rid) {
  while (*it_ != *end_) {
    auto [r, bytes] = **it_;
    ++(*it_);
    Tuple t = Tuple::Deserialize(table_->schema, bytes);
    if (EvalPredicates(preds_, t, table_->schema)) {
      MaybeLockShared(ctx_, r);
      *out = t;
      *rid = r;
      return true;
    }
  }
  return false;
}

// ---- IndexScan ----
IndexScanExecutor::IndexScanExecutor(ExecutorContext *ctx, TableInfo *table, IndexInfo *index,
                                     IndexKey low, IndexKey high, std::vector<Predicate> residual)
    : ctx_(ctx), table_(table), index_(index), low_(low), high_(high), residual_(std::move(residual)) {}

void IndexScanExecutor::Init() {
  rids_.clear();
  pos_ = 0;
  index_->tree->RangeScan(low_, high_, [&](const IndexKey &, const RID &r) { rids_.push_back(r); });
}

bool IndexScanExecutor::Next(Tuple *out, RID *rid) {
  while (pos_ < rids_.size()) {
    RID r = rids_[pos_++];
    std::string bytes;
    if (!table_->heap->GetTuple(r, &bytes)) continue;  // deleted since scan
    Tuple t = Tuple::Deserialize(table_->schema, bytes);
    if (EvalPredicates(residual_, t, table_->schema)) {
      MaybeLockShared(ctx_, r);
      *out = t;
      *rid = r;
      return true;
    }
  }
  return false;
}

// ---- Filter ----
FilterExecutor::FilterExecutor(std::unique_ptr<Executor> child, std::vector<Predicate> preds)
    : child_(std::move(child)), preds_(std::move(preds)) {}

bool FilterExecutor::Next(Tuple *out, RID *rid) {
  Tuple t;
  RID r;
  while (child_->Next(&t, &r)) {
    if (EvalPredicates(preds_, t, child_->GetOutputSchema())) { *out = t; *rid = r; return true; }
  }
  return false;
}

// ---- NestedLoopJoin ----
NestedLoopJoinExecutor::NestedLoopJoinExecutor(
    std::unique_ptr<Executor> left, int left_key_col, int right_key_col,
    std::function<std::unique_ptr<Executor>(const Value &)> right_factory, Schema output_schema)
    : left_(std::move(left)), left_key_col_(left_key_col), right_key_col_(right_key_col),
      right_factory_(std::move(right_factory)), output_schema_(std::move(output_schema)) {}

void NestedLoopJoinExecutor::Init() {
  left_->Init();
  has_left_ = AdvanceLeft();
}

bool NestedLoopJoinExecutor::AdvanceLeft() {
  RID r;
  if (!left_->Next(&left_tuple_, &r)) return false;
  right_ = right_factory_(left_tuple_.value(left_key_col_));
  right_->Init();
  return true;
}

bool NestedLoopJoinExecutor::Next(Tuple *out, RID *rid) {
  while (has_left_) {
    Tuple rt;
    RID rr;
    while (right_->Next(&rt, &rr)) {
      // equi-join condition
      if (left_tuple_.value(left_key_col_).Compare(rt.value(right_key_col_)) != 0) continue;
      std::vector<Value> vals = left_tuple_.values();
      for (auto &v : rt.values()) vals.push_back(v);
      *out = Tuple(std::move(vals));
      *rid = RID();
      return true;
    }
    has_left_ = AdvanceLeft();
  }
  return false;
}

// ---- Projection ----
ProjectionExecutor::ProjectionExecutor(std::unique_ptr<Executor> child, const std::vector<SelectItem> &items)
    : child_(std::move(child)), items_(items) {
  const Schema &in = child_->GetOutputSchema();
  std::vector<Column> cols;
  for (auto &item : items_) {
    if (item.is_star) {
      for (auto &c : in.columns()) { cols.push_back(c); col_idx_.push_back(-2); }  // -2: expand-star marker handled below
    } else {
      int ci = in.IndexOf(item.col);
      if (ci < 0) throw DBException("unknown column in projection: " + item.col);
      col_idx_.push_back(ci);
      Column c = in.column(ci);
      if (!item.alias.empty()) c.name = item.alias;
      cols.push_back(c);
    }
  }
  output_schema_ = Schema(cols);
}

void ProjectionExecutor::Init() { child_->Init(); }

bool ProjectionExecutor::Next(Tuple *out, RID *rid) {
  Tuple t;
  RID r;
  if (!child_->Next(&t, &r)) return false;
  std::vector<Value> vals;
  size_t star_cursor = 0;
  const Schema &in = child_->GetOutputSchema();
  for (size_t i = 0; i < items_.size(); i++) {
    if (items_[i].is_star) {
      for (size_t c = 0; c < in.column_count(); c++) vals.push_back(t.value(c));
      (void)star_cursor;
    } else {
      vals.push_back(t.value(col_idx_[i]));
    }
  }
  *out = Tuple(std::move(vals));
  *rid = r;
  return true;
}

// ---- Aggregate ----
AggregateExecutor::AggregateExecutor(std::unique_ptr<Executor> child, std::vector<SelectItem> items,
                                     std::vector<std::string> group_by)
    : child_(std::move(child)), items_(std::move(items)), group_by_(std::move(group_by)) {
  // Output schema: group columns keep their type; aggregates are BIGINT
  // (COUNT/SUM) reported numerically. MIN/MAX keep the source column type.
  const Schema &in = child_->GetOutputSchema();
  auto default_name = [](const SelectItem &it) -> std::string {
    switch (it.agg) {
      case AggType::COUNT_STAR: return "count(*)";
      case AggType::COUNT:      return "count(" + it.col + ")";
      case AggType::SUM:        return "sum(" + it.col + ")";
      case AggType::AVG:        return "avg(" + it.col + ")";
      case AggType::MIN:        return "min(" + it.col + ")";
      case AggType::MAX:        return "max(" + it.col + ")";
      default:                  return it.col;
    }
  };
  std::vector<Column> cols;
  for (auto &item : items_) {
    std::string name = !item.alias.empty() ? item.alias : default_name(item);
    if (item.agg == AggType::NONE || item.agg == AggType::MIN || item.agg == AggType::MAX) {
      int ci = in.IndexOf(item.col);
      cols.push_back({name, ci >= 0 ? in.column(ci).type : TypeId::BIGINT, 0});
    } else {
      cols.push_back({name, TypeId::BIGINT, 0});  // COUNT/SUM/AVG are numeric
    }
  }
  output_schema_ = Schema(cols);
}

void AggregateExecutor::Init() {
  child_->Init();
  const Schema &in = child_->GetOutputSchema();

  struct AggState { int64_t count = 0; int64_t sum = 0; Value mn; Value mx; bool seen = false; };
  // group key -> per-item state
  std::map<std::vector<std::string>, std::vector<AggState>> groups;
  std::map<std::vector<std::string>, std::vector<Value>> group_key_vals;

  Tuple t;
  RID r;
  while (child_->Next(&t, &r)) {
    std::vector<std::string> key;
    std::vector<Value> keyvals;
    for (auto &g : group_by_) {
      int ci = in.IndexOf(g);
      key.push_back(t.value(ci).ToString());
      keyvals.push_back(t.value(ci));
    }
    auto &states = groups[key];
    if (states.empty()) { states.resize(items_.size()); group_key_vals[key] = keyvals; }
    for (size_t i = 0; i < items_.size(); i++) {
      auto &st = states[i];
      const SelectItem &item = items_[i];
      if (item.agg == AggType::COUNT_STAR) { st.count++; continue; }
      if (item.agg == AggType::NONE) continue;
      int ci = in.IndexOf(item.col);
      const Value &v = t.value(ci);
      st.count++;
      st.sum += v.as_int();
      if (!st.seen) { st.mn = v; st.mx = v; st.seen = true; }
      else { if (v.Compare(st.mn) < 0) st.mn = v; if (v.Compare(st.mx) > 0) st.mx = v; }
    }
  }

  // No-group aggregate over an empty table still yields one row.
  if (groups.empty() && group_by_.empty()) {
    groups[{}].resize(items_.size());
    group_key_vals[{}] = {};
  }

  for (auto &[key, states] : groups) {
    std::vector<Value> row;
    size_t gpos = 0;
    for (size_t i = 0; i < items_.size(); i++) {
      const SelectItem &item = items_[i];
      const AggState &st = states[i];
      switch (item.agg) {
        case AggType::NONE:       row.push_back(group_key_vals[key][gpos++]); break;
        case AggType::COUNT_STAR:
        case AggType::COUNT:      row.push_back(Value::BigInt(st.count)); break;
        case AggType::SUM:        row.push_back(Value::BigInt(st.sum)); break;
        case AggType::AVG:        row.push_back(Value::BigInt(st.count ? st.sum / st.count : 0)); break;
        case AggType::MIN:        row.push_back(st.seen ? st.mn : Value::MakeNull(TypeId::BIGINT)); break;
        case AggType::MAX:        row.push_back(st.seen ? st.mx : Value::MakeNull(TypeId::BIGINT)); break;
      }
    }
    result_rows_.emplace_back(std::move(row));
  }
}

bool AggregateExecutor::Next(Tuple *out, RID *rid) {
  if (pos_ >= result_rows_.size()) return false;
  *out = result_rows_[pos_++];
  *rid = RID();
  return true;
}

// ---- Insert ----
InsertExecutor::InsertExecutor(ExecutorContext *ctx, TableInfo *table, std::vector<Tuple> rows)
    : ctx_(ctx), table_(table), rows_(std::move(rows)) {}

void InsertExecutor::Init() {
  for (auto &row : rows_) {
    std::string bytes = row.Serialize(table_->schema);
    RID rid;
    if (!table_->heap->InsertTuple(bytes, &rid, ctx_->txn)) throw DBException("insert failed (row too large?)");
    MaybeLockExclusive(ctx_, rid);
    for (auto &idx : table_->indexes) {
      if (!idx->tree->Insert(IndexKey::FromValue(row.value(idx->key_col)), rid)) {
        throw DBException("duplicate key for index " + idx->name);
      }
    }
    affected_++;
  }
  ctx_->catalog->MarkStatsDirty(table_->name);
}

// ---- Delete ----
DeleteExecutor::DeleteExecutor(ExecutorContext *ctx, TableInfo *table, std::unique_ptr<Executor> child)
    : ctx_(ctx), table_(table), child_(std::move(child)) {}

void DeleteExecutor::Init() {
  child_->Init();
  Tuple t;
  RID rid;
  std::vector<std::pair<RID, Tuple>> targets;
  while (child_->Next(&t, &rid)) targets.emplace_back(rid, t);
  for (auto &[r, tup] : targets) {
    MaybeLockExclusive(ctx_, r);
    table_->heap->DeleteTuple(r, ctx_->txn);
    for (auto &idx : table_->indexes) {
      // Remove the exact (key, rid) so non-unique secondary indexes only drop
      // this row's entry, not another row that shares the key value.
      idx->tree->Remove(IndexKey::FromValue(tup.value(idx->key_col)), r);
    }
    affected_++;
  }
  ctx_->catalog->MarkStatsDirty(table_->name);
}

}  // namespace minidb
