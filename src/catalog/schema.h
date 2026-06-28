#pragma once
#include <string>
#include <vector>

#include "common/types.h"

namespace minidb {

// One column definition. `length` is the max byte length for VARCHAR; ignored
// for fixed-width types.
struct Column {
  std::string name;
  TypeId type;
  uint32_t length{0};
};

// Ordered set of columns plus an optional primary-key column index.
class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<Column> cols, int pk = -1)
      : columns_(std::move(cols)), pk_index_(pk) {}

  size_t column_count() const { return columns_.size(); }
  const Column &column(size_t i) const { return columns_[i]; }
  const std::vector<Column> &columns() const { return columns_; }
  int pk_index() const { return pk_index_; }
  void set_pk_index(int i) { pk_index_ = i; }

  // Resolve a column name to its index, or -1 if absent. Optional table
  // qualifier ("t.col") is stripped before matching.
  int IndexOf(const std::string &name) const {
    std::string n = name;
    auto dot = n.find('.');
    if (dot != std::string::npos) n = n.substr(dot + 1);
    for (size_t i = 0; i < columns_.size(); i++) {
      if (columns_[i].name == n) return static_cast<int>(i);
    }
    return -1;
  }

 private:
  std::vector<Column> columns_;
  int pk_index_{-1};
};

}  // namespace minidb
