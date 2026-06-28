#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "recovery/log_record.h"

namespace minidb {

// Append-only Write-Ahead Log. Assigns monotonic LSNs, buffers records, and
// flushes to disk on demand. Durability points: force-log-at-commit (callers
// flush after appending a COMMIT) and the write-ahead rule (BufferPool flushes
// the log up to a page's LSN before writing that page).
class LogManager {
 public:
  explicit LogManager(const std::string &log_file);
  ~LogManager();

  // Append a record, stamping it with the next LSN (set into rec.lsn). Returns the LSN.
  lsn_t Append(LogRecord &rec);

  // Ensure all records with lsn <= target are durable. target<0 flushes everything buffered.
  void Flush(lsn_t target = -1);

  lsn_t GetNextLSN() const { return next_lsn_; }

  // Read every record from the log file (used by recovery). Ordered by LSN.
  std::vector<LogRecord> ReadAll();

 private:
  std::string file_name_;
  std::fstream out_;
  lsn_t next_lsn_{0};
  lsn_t flushed_lsn_{INVALID_LSN};
  std::string buffer_;          // unflushed serialized records
  std::mutex latch_;
};

}  // namespace minidb
