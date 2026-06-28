#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "common/config.h"
#include "common/rid.h"

namespace minidb {

enum class LogType : uint8_t {
  INVALID = 0,
  BEGIN,
  COMMIT,
  ABORT,
  INSERT,       // new_tuple inserted at rid
  MARK_DELETE,  // old_tuple deleted at rid (before image kept for undo)
  UPDATE,       // old_tuple -> new_tuple at rid
  NEWPAGE,      // heap allocated+linked a page: rid.page_id=new, rid.slot=prev page
  CHECKPOINT
};

// A single WAL record. Data-bearing records (INSERT/DELETE/UPDATE) carry the
// table oid, RID, and before/after tuple images needed for redo and undo.
struct LogRecord {
  lsn_t lsn{INVALID_LSN};
  lsn_t prev_lsn{INVALID_LSN};   // previous LSN of the same txn (undo chain)
  txn_id_t txn_id{INVALID_TXN_ID};
  LogType type{LogType::INVALID};
  oid_t table_oid{-1};
  RID rid{};
  std::string old_tuple;
  std::string new_tuple;

  // ---- (de)serialization to the WAL byte stream ----
  std::string Serialize() const {
    std::string buf;
    auto put = [&](const void *p, size_t n) {
      buf.append(static_cast<const char *>(p), n);
    };
    auto put_str = [&](const std::string &s) {
      uint32_t len = static_cast<uint32_t>(s.size());
      put(&len, 4);
      put(s.data(), len);
    };
    uint8_t t = static_cast<uint8_t>(type);
    put(&lsn, 8); put(&prev_lsn, 8); put(&txn_id, 8); put(&t, 1);
    put(&table_oid, 4); put(&rid.page_id, 4); put(&rid.slot, 4);
    put_str(old_tuple); put_str(new_tuple);

    // Frame the record with a leading total-length so the reader can scan.
    uint32_t total = static_cast<uint32_t>(buf.size());
    std::string framed;
    framed.append(reinterpret_cast<const char *>(&total), 4);
    framed += buf;
    return framed;
  }

  // Parse one record from `data` starting at *pos (which already points past the
  // 4-byte length prefix). Returns false on truncation.
  static bool Deserialize(const std::string &payload, LogRecord *out) {
    size_t pos = 0;
    auto get = [&](void *p, size_t n) -> bool {
      if (pos + n > payload.size()) return false;
      std::memcpy(p, payload.data() + pos, n);
      pos += n;
      return true;
    };
    auto get_str = [&](std::string *s) -> bool {
      uint32_t len;
      if (!get(&len, 4)) return false;
      if (pos + len > payload.size()) return false;
      s->assign(payload.data() + pos, len);
      pos += len;
      return true;
    };
    uint8_t t;
    if (!get(&out->lsn, 8) || !get(&out->prev_lsn, 8) || !get(&out->txn_id, 8) ||
        !get(&t, 1) || !get(&out->table_oid, 4) || !get(&out->rid.page_id, 4) ||
        !get(&out->rid.slot, 4) || !get_str(&out->old_tuple) ||
        !get_str(&out->new_tuple)) {
      return false;
    }
    out->type = static_cast<LogType>(t);
    return true;
  }
};

}  // namespace minidb
