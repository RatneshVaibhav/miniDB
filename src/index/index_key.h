#pragma once
#include <cstring>
#include <string>

#include "common/types.h"

namespace minidb {

// Fixed-width, order-preserving key encoding used by the B+Tree. All keys in a
// given index share a column type, so memcmp over the 32-byte encoding yields
// the correct logical order:
//   - integers/booleans: 8-byte big-endian with the sign bit flipped so
//     two's-complement values sort correctly under unsigned byte comparison.
//   - varchar: raw bytes, left-aligned, zero-padded (keys > 32 bytes are
//     truncated — documented limitation).
struct IndexKey {
  static constexpr int LEN = 32;
  char data[LEN];

  IndexKey() { std::memset(data, 0, LEN); }

  static IndexKey FromValue(const Value &v) {
    IndexKey k;
    if (v.type() == TypeId::VARCHAR) {
      const std::string &s = v.as_string();
      size_t n = std::min<size_t>(LEN, s.size());
      std::memcpy(k.data, s.data(), n);
    } else {
      uint64_t u = static_cast<uint64_t>(v.as_int()) ^ 0x8000000000000000ULL;
      for (int i = 0; i < 8; i++) {
        k.data[i] = static_cast<char>((u >> (8 * (7 - i))) & 0xff);
      }
    }
    return k;
  }

  int Compare(const IndexKey &o) const { return std::memcmp(data, o.data, LEN); }
  bool operator<(const IndexKey &o) const { return Compare(o) < 0; }
  bool operator==(const IndexKey &o) const { return Compare(o) == 0; }
};

}  // namespace minidb
