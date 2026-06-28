#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "catalog/catalog.h"
#include "common/rid.h"
#include "record/tuple.h"
#include "sql/ast.h"
#include "storage/table_heap.h"

namespace minidb {

class Transaction;
class LockManager;

// Shared state threaded through a plan's operators.
struct ExecutorContext {
  Catalog *catalog;
  Transaction *txn{nullptr};     // null in autocommit read paths
  LockManager *lock_mgr{nullptr};
};

// Evaluate one comparison predicate against a tuple under a schema.
bool EvalPredicate(const Predicate &p, const Tuple &t, const Schema &schema);
// Evaluate a conjunction (all must hold).
bool EvalPredicates(const std::vector<Predicate> &ps, const Tuple &t, const Schema &schema);

// Volcano-style iterator. Init() then repeated Next() until it returns false.
class Executor {
 public:
  virtual ~Executor() = default;
  virtual void Init() = 0;
  virtual bool Next(Tuple *out, RID *rid) = 0;
  virtual const Schema &GetOutputSchema() const = 0;
  virtual size_t GetAffected() const { return 0; }  // for DML executors
};

// Sequential scan over a heap with optional pushed-down predicates.
class SeqScanExecutor : public Executor {
 public:
  SeqScanExecutor(ExecutorContext *ctx, TableInfo *table, std::vector<Predicate> preds);
  void Init() override;
  bool Next(Tuple *out, RID *rid) override;
  const Schema &GetOutputSchema() const override { return table_->schema; }

 private:
  ExecutorContext *ctx_;
  TableInfo *table_;
  std::vector<Predicate> preds_;
  std::unique_ptr<TableHeap::Iterator> it_;
  std::unique_ptr<TableHeap::Iterator> end_;
};

// Index scan over a B+Tree for keys in [low, high], with optional residual predicates.
class IndexScanExecutor : public Executor {
 public:
  IndexScanExecutor(ExecutorContext *ctx, TableInfo *table, IndexInfo *index,
                    IndexKey low, IndexKey high, std::vector<Predicate> residual);
  void Init() override;
  bool Next(Tuple *out, RID *rid) override;
  const Schema &GetOutputSchema() const override { return table_->schema; }

 private:
  ExecutorContext *ctx_;
  TableInfo *table_;
  IndexInfo *index_;
  IndexKey low_, high_;
  std::vector<Predicate> residual_;
  std::vector<RID> rids_;
  size_t pos_{0};
};

// Filter on top of a child operator.
class FilterExecutor : public Executor {
 public:
  FilterExecutor(std::unique_ptr<Executor> child, std::vector<Predicate> preds);
  void Init() override { child_->Init(); }
  bool Next(Tuple *out, RID *rid) override;
  const Schema &GetOutputSchema() const override { return child_->GetOutputSchema(); }

 private:
  std::unique_ptr<Executor> child_;
  std::vector<Predicate> preds_;
};

// Nested-loop join. The right side is built per left row via `right_factory`,
// which receives the left join-key value (an index-NLJ factory uses it to probe
// an index; a plain-NLJ factory ignores it and rescans).
class NestedLoopJoinExecutor : public Executor {
 public:
  NestedLoopJoinExecutor(std::unique_ptr<Executor> left, int left_key_col, int right_key_col,
                         std::function<std::unique_ptr<Executor>(const Value &)> right_factory,
                         Schema output_schema);
  void Init() override;
  bool Next(Tuple *out, RID *rid) override;
  const Schema &GetOutputSchema() const override { return output_schema_; }

 private:
  bool AdvanceLeft();
  std::unique_ptr<Executor> left_;
  int left_key_col_, right_key_col_;
  std::function<std::unique_ptr<Executor>(const Value &)> right_factory_;
  Schema output_schema_;
  std::unique_ptr<Executor> right_;
  Tuple left_tuple_;
  bool has_left_{false};
};

// Projection / aggregation producing the final output rows.
class ProjectionExecutor : public Executor {
 public:
  ProjectionExecutor(std::unique_ptr<Executor> child, const std::vector<SelectItem> &items);
  void Init() override;
  bool Next(Tuple *out, RID *rid) override;
  const Schema &GetOutputSchema() const override { return output_schema_; }

 private:
  std::unique_ptr<Executor> child_;
  std::vector<SelectItem> items_;
  Schema output_schema_;
  std::vector<int> col_idx_;   // resolved child-column index per output item
};

class AggregateExecutor : public Executor {
 public:
  AggregateExecutor(std::unique_ptr<Executor> child, std::vector<SelectItem> items,
                    std::vector<std::string> group_by);
  void Init() override;
  bool Next(Tuple *out, RID *rid) override;
  const Schema &GetOutputSchema() const override { return output_schema_; }

 private:
  std::unique_ptr<Executor> child_;
  std::vector<SelectItem> items_;
  std::vector<std::string> group_by_;
  Schema output_schema_;
  std::vector<Tuple> result_rows_;
  size_t pos_{0};
};

// INSERT: writes rows to the heap, maintains indexes, logs via the heap.
class InsertExecutor : public Executor {
 public:
  InsertExecutor(ExecutorContext *ctx, TableInfo *table, std::vector<Tuple> rows);
  void Init() override;
  bool Next(Tuple *, RID *) override { return false; }
  const Schema &GetOutputSchema() const override { return table_->schema; }
  size_t GetAffected() const override { return affected_; }

 private:
  ExecutorContext *ctx_;
  TableInfo *table_;
  std::vector<Tuple> rows_;
  size_t affected_{0};
};

// DELETE: drains a child scan and removes each produced RID + index entries.
class DeleteExecutor : public Executor {
 public:
  DeleteExecutor(ExecutorContext *ctx, TableInfo *table, std::unique_ptr<Executor> child);
  void Init() override;
  bool Next(Tuple *, RID *) override { return false; }
  const Schema &GetOutputSchema() const override { return table_->schema; }
  size_t GetAffected() const override { return affected_; }

 private:
  ExecutorContext *ctx_;
  TableInfo *table_;
  std::unique_ptr<Executor> child_;
  size_t affected_{0};
};

}  // namespace minidb
