#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/exception.h"

namespace minidb {

// Supported logical column types. INTEGER is 4 bytes, BIGINT 8 bytes,
// BOOLEAN 1 byte, VARCHAR is variable-length (length-prefixed when serialized).
enum class TypeId : uint8_t { INVALID = 0, BOOLEAN, INTEGER, BIGINT, VARCHAR };

inline std::string TypeIdToString(TypeId t) {
  switch (t) {
    case TypeId::BOOLEAN: return "BOOLEAN";
    case TypeId::INTEGER: return "INTEGER";
    case TypeId::BIGINT:  return "BIGINT";
    case TypeId::VARCHAR: return "VARCHAR";
    default:              return "INVALID";
  }
}

inline TypeId StringToTypeId(const std::string &s) {
  if (s == "BOOLEAN" || s == "BOOL") return TypeId::BOOLEAN;
  if (s == "INT" || s == "INTEGER")  return TypeId::INTEGER;
  if (s == "BIGINT" || s == "LONG")  return TypeId::BIGINT;
  if (s == "VARCHAR" || s == "TEXT" || s == "STRING") return TypeId::VARCHAR;
  return TypeId::INVALID;
}

// A single SQL value. Numeric/boolean values are stored in `i_`, strings in `s_`.
// Supports a NULL marker so columns can hold absent values.
class Value {
 public:
  Value() : type_(TypeId::INVALID), is_null_(true), i_(0) {}

  static Value MakeNull(TypeId t) { Value v; v.type_ = t; v.is_null_ = true; return v; }
  static Value Bool(bool b)      { Value v; v.type_ = TypeId::BOOLEAN; v.is_null_ = false; v.i_ = b ? 1 : 0; return v; }
  static Value Int(int32_t x)    { Value v; v.type_ = TypeId::INTEGER; v.is_null_ = false; v.i_ = x; return v; }
  static Value BigInt(int64_t x) { Value v; v.type_ = TypeId::BIGINT;  v.is_null_ = false; v.i_ = x; return v; }
  static Value Varchar(std::string s) {
    Value v; v.type_ = TypeId::VARCHAR; v.is_null_ = false; v.s_ = std::move(s); return v;
  }

  TypeId type() const { return type_; }
  bool is_null() const { return is_null_; }

  int64_t as_int() const { return i_; }
  bool as_bool() const { return i_ != 0; }
  const std::string &as_string() const { return s_; }

  // Three-way compare. Both values must be the same logical category.
  // Returns <0, 0, >0. NULLs sort before everything else.
  int Compare(const Value &o) const {
    if (is_null_ || o.is_null_) {
      if (is_null_ && o.is_null_) return 0;
      return is_null_ ? -1 : 1;
    }
    if (type_ == TypeId::VARCHAR || o.type_ == TypeId::VARCHAR) {
      return s_.compare(o.s_);
    }
    if (i_ < o.i_) return -1;
    if (i_ > o.i_) return 1;
    return 0;
  }

  bool operator==(const Value &o) const { return Compare(o) == 0; }
  bool operator<(const Value &o) const { return Compare(o) < 0; }

  std::string ToString() const {
    if (is_null_) return "NULL";
    switch (type_) {
      case TypeId::BOOLEAN: return i_ ? "true" : "false";
      case TypeId::INTEGER:
      case TypeId::BIGINT:  return std::to_string(i_);
      case TypeId::VARCHAR: return s_;
      default:              return "?";
    }
  }

 private:
  TypeId type_;
  bool is_null_;
  int64_t i_;        // BOOLEAN/INTEGER/BIGINT payload
  std::string s_;    // VARCHAR payload
};

}  // namespace minidb
