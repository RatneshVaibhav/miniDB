#include "lsm/sstable.h"

#include <cstring>

namespace minidb {

static constexpr uint32_t kMagic = 0x53535401;  // "SST\1"

static void PutU32(std::ofstream &o, uint32_t v) { o.write(reinterpret_cast<char *>(&v), 4); }

std::unique_ptr<SSTable> SSTable::Create(const std::string &path,
                                         const std::map<std::string, LsmEntry> &data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  PutU32(out, kMagic);
  PutU32(out, static_cast<uint32_t>(data.size()));
  for (auto &[k, e] : data) {
    PutU32(out, static_cast<uint32_t>(k.size()));
    out.write(k.data(), k.size());
    uint8_t type = e.tombstone ? 1 : 0;
    out.write(reinterpret_cast<char *>(&type), 1);
    PutU32(out, static_cast<uint32_t>(e.value.size()));
    out.write(e.value.data(), e.value.size());
  }
  out.flush();
  out.close();
  return Open(path);
}

std::unique_ptr<SSTable> SSTable::Open(const std::string &path) {
  auto t = std::unique_ptr<SSTable>(new SSTable());
  t->path_ = path;
  t->in_.open(path, std::ios::binary);
  if (!t->in_.is_open()) return nullptr;

  uint32_t magic, count;
  t->in_.read(reinterpret_cast<char *>(&magic), 4);
  t->in_.read(reinterpret_cast<char *>(&count), 4);
  t->bloom_ = std::make_unique<BloomFilter>(count);

  for (uint32_t i = 0; i < count; i++) {
    uint64_t offset = static_cast<uint64_t>(t->in_.tellg());
    uint32_t klen;
    t->in_.read(reinterpret_cast<char *>(&klen), 4);
    std::string key(klen, '\0');
    t->in_.read(key.data(), klen);
    uint8_t type;
    t->in_.read(reinterpret_cast<char *>(&type), 1);
    uint32_t vlen;
    t->in_.read(reinterpret_cast<char *>(&vlen), 4);
    t->in_.seekg(vlen, std::ios::cur);  // skip value; we have the offset
    t->index_[key] = offset;
    t->bloom_->Add(key);
  }
  t->in_.clear();
  t->in_.seekg(0, std::ios::end);
  t->file_size_ = static_cast<uint64_t>(t->in_.tellg());
  return t;
}

LsmEntry SSTable::ReadAt(uint64_t offset) {
  in_.clear();
  in_.seekg(static_cast<std::streamoff>(offset));
  uint32_t klen;
  in_.read(reinterpret_cast<char *>(&klen), 4);
  in_.seekg(klen, std::ios::cur);  // skip key
  uint8_t type;
  in_.read(reinterpret_cast<char *>(&type), 1);
  uint32_t vlen;
  in_.read(reinterpret_cast<char *>(&vlen), 4);
  std::string val(vlen, '\0');
  in_.read(val.data(), vlen);
  return LsmEntry{val, type == 1};
}

std::optional<LsmEntry> SSTable::Get(const std::string &key) {
  if (!bloom_->MaybeContains(key)) return std::nullopt;  // fast negative
  auto it = index_.find(key);
  if (it == index_.end()) return std::nullopt;
  return ReadAt(it->second);
}

std::vector<std::pair<std::string, LsmEntry>> SSTable::ScanAll() {
  std::vector<std::pair<std::string, LsmEntry>> out;
  out.reserve(index_.size());
  for (auto &[k, off] : index_) out.emplace_back(k, ReadAt(off));  // index_ is sorted
  return out;
}

}  // namespace minidb
