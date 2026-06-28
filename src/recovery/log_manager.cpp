#include "recovery/log_manager.h"

#include "common/exception.h"

namespace minidb {

LogManager::LogManager(const std::string &log_file) : file_name_(log_file) {
  // Determine next LSN by replaying existing records (count them).
  auto existing = ReadAll();
  for (auto &r : existing) next_lsn_ = std::max(next_lsn_, r.lsn + 1);
  flushed_lsn_ = next_lsn_ - 1;

  out_.open(file_name_, std::ios::binary | std::ios::out | std::ios::app);
  if (!out_.is_open()) throw DBException("LogManager: cannot open " + file_name_);
}

LogManager::~LogManager() {
  Flush();
  if (out_.is_open()) out_.close();
}

lsn_t LogManager::Append(LogRecord &rec) {
  std::lock_guard<std::mutex> g(latch_);
  rec.lsn = next_lsn_++;
  buffer_ += rec.Serialize();
  return rec.lsn;
}

void LogManager::Flush(lsn_t target) {
  std::lock_guard<std::mutex> g(latch_);
  if (buffer_.empty()) return;
  // We only ever buffer in append order, so flushing everything buffered always
  // satisfies "make lsn <= target durable".
  (void)target;
  out_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
  out_.flush();
  buffer_.clear();
  flushed_lsn_ = next_lsn_ - 1;
}

std::vector<LogRecord> LogManager::ReadAll() {
  std::vector<LogRecord> records;
  std::ifstream in(file_name_, std::ios::binary);
  if (!in.is_open()) return records;
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  size_t pos = 0;
  while (pos + 4 <= all.size()) {
    uint32_t total;
    std::memcpy(&total, all.data() + pos, 4);
    pos += 4;
    if (pos + total > all.size()) break;  // torn write at tail -> stop
    LogRecord rec;
    std::string payload(all.data() + pos, total);
    if (!LogRecord::Deserialize(payload, &rec)) break;
    records.push_back(std::move(rec));
    pos += total;
  }
  return records;
}

}  // namespace minidb
