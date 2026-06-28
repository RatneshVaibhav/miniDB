#pragma once
#include <fstream>
#include <mutex>
#include <string>

#include "common/config.h"

namespace minidb {

// Owns the single database file and does fixed-size page I/O by page id.
// Page allocation is a simple high-water mark persisted implicitly by file size.
class DiskManager {
 public:
  explicit DiskManager(const std::string &db_file);
  ~DiskManager();

  void ReadPage(page_id_t page_id, char *dst);
  void WritePage(page_id_t page_id, const char *src);
  page_id_t AllocatePage();          // returns a fresh page id
  page_id_t NumPages() const { return num_pages_; }
  void Flush();                      // fsync the data file

 private:
  std::string file_name_;
  std::fstream io_;
  page_id_t num_pages_{0};
  std::mutex latch_;
};

}  // namespace minidb
