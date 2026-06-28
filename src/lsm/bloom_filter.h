#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace minidb {

// Bloom filter for fast negative lookups in an SSTable. Uses double hashing to
// synthesize k hash functions from two base hashes.
class BloomFilter {
 public:
  explicit BloomFilter(size_t expected, int k = 4) : k_(k) {
    size_t bits = std::max<size_t>(64, expected * 10);  // ~10 bits/key
    bits_.assign((bits + 63) / 64, 0);
    nbits_ = bits_.size() * 64;
  }

  void Add(const std::string &key) {
    uint64_t h1 = Hash1(key), h2 = Hash2(key);
    for (int i = 0; i < k_; i++) Set((h1 + static_cast<uint64_t>(i) * h2) % nbits_);
  }

  bool MaybeContains(const std::string &key) const {
    uint64_t h1 = Hash1(key), h2 = Hash2(key);
    for (int i = 0; i < k_; i++) {
      if (!Get((h1 + static_cast<uint64_t>(i) * h2) % nbits_)) return false;
    }
    return true;
  }

  const std::vector<uint64_t> &words() const { return bits_; }
  void LoadWords(std::vector<uint64_t> w) { bits_ = std::move(w); nbits_ = bits_.size() * 64; }

 private:
  void Set(uint64_t i) { bits_[i / 64] |= (1ULL << (i % 64)); }
  bool Get(uint64_t i) const { return (bits_[i / 64] >> (i % 64)) & 1ULL; }
  static uint64_t Hash1(const std::string &s) { return std::hash<std::string>{}(s); }
  static uint64_t Hash2(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (char c : s) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ULL; }
    return h | 1;  // ensure odd (good stride)
  }

  std::vector<uint64_t> bits_;
  size_t nbits_;
  int k_;
};

}  // namespace minidb
