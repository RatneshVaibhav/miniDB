#pragma once
#include <string>
#include <vector>

#include "catalog/schema.h"
#include "common/types.h"

namespace minidb {

// An in-memory row: one Value per schema column. Serialization format:
//   [null bitmap: ceil(n/8) bytes] then, per non-null column:
//     BOOLEAN -> 1 byte, INTEGER -> 4 bytes, BIGINT -> 8 bytes,
//     VARCHAR -> u32 length + bytes.
class Tuple {
 public:
  Tuple() = default;
  explicit Tuple(std::vector<Value> values) : values_(std::move(values)) {}

  size_t size() const { return values_.size(); }
  const Value &value(size_t i) const { return values_[i]; }
  const std::vector<Value> &values() const { return values_; }

  std::string Serialize(const Schema &schema) const {
    size_t n = schema.column_count();
    size_t bitmap_bytes = (n + 7) / 8;
    std::string out(bitmap_bytes, '\0');
    for (size_t i = 0; i < n; i++) {
      const Value &v = values_[i];
      if (v.is_null()) {
        out[i / 8] |= static_cast<char>(1 << (i % 8));
        continue;
      }
      switch (schema.column(i).type) {
        case TypeId::BOOLEAN: { uint8_t b = v.as_bool() ? 1 : 0; out.append(reinterpret_cast<char *>(&b), 1); break; }
        case TypeId::INTEGER: { int32_t x = static_cast<int32_t>(v.as_int()); out.append(reinterpret_cast<char *>(&x), 4); break; }
        case TypeId::BIGINT:  { int64_t x = v.as_int(); out.append(reinterpret_cast<char *>(&x), 8); break; }
        case TypeId::VARCHAR: { const std::string &s = v.as_string(); uint32_t len = static_cast<uint32_t>(s.size());
                                out.append(reinterpret_cast<char *>(&len), 4); out += s; break; }
        default: break;
      }
    }
    return out;
  }

  static Tuple Deserialize(const Schema &schema, const std::string &bytes) {
    size_t n = schema.column_count();
    size_t bitmap_bytes = (n + 7) / 8;
    std::vector<Value> vals;
    vals.reserve(n);
    size_t pos = bitmap_bytes;
    for (size_t i = 0; i < n; i++) {
      bool is_null = (bytes[i / 8] >> (i % 8)) & 1;
      TypeId t = schema.column(i).type;
      if (is_null) { vals.push_back(Value::MakeNull(t)); continue; }
      switch (t) {
        case TypeId::BOOLEAN: { uint8_t b; std::memcpy(&b, bytes.data() + pos, 1); pos += 1; vals.push_back(Value::Bool(b != 0)); break; }
        case TypeId::INTEGER: { int32_t x; std::memcpy(&x, bytes.data() + pos, 4); pos += 4; vals.push_back(Value::Int(x)); break; }
        case TypeId::BIGINT:  { int64_t x; std::memcpy(&x, bytes.data() + pos, 8); pos += 8; vals.push_back(Value::BigInt(x)); break; }
        case TypeId::VARCHAR: { uint32_t len; std::memcpy(&len, bytes.data() + pos, 4); pos += 4;
                                vals.push_back(Value::Varchar(bytes.substr(pos, len))); pos += len; break; }
        default: vals.push_back(Value::MakeNull(t)); break;
      }
    }
    return Tuple(std::move(vals));
  }

 private:
  std::vector<Value> values_;
};

}  // namespace minidb
