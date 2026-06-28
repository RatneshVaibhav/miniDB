#include "storage/disk_manager.h"

#include <cstring>
#include <stdexcept>

#include "common/exception.h"

namespace minidb {

DiskManager::DiskManager(const std::string &db_file) : file_name_(db_file) {
  // Open for read+write, creating the file if absent.
  io_.open(file_name_, std::ios::binary | std::ios::in | std::ios::out);
  if (!io_.is_open()) {
    io_.clear();
    io_.open(file_name_, std::ios::binary | std::ios::trunc | std::ios::out);
    io_.close();
    io_.open(file_name_, std::ios::binary | std::ios::in | std::ios::out);
  }
  if (!io_.is_open()) throw DBException("DiskManager: cannot open " + file_name_);

  io_.seekg(0, std::ios::end);
  std::streampos size = io_.tellg();
  num_pages_ = (size <= 0) ? 0 : static_cast<page_id_t>(size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
  if (io_.is_open()) { io_.flush(); io_.close(); }
}

void DiskManager::ReadPage(page_id_t page_id, char *dst) {
  std::lock_guard<std::mutex> g(latch_);
  size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;
  io_.seekg(offset);
  io_.read(dst, PAGE_SIZE);
  size_t read = io_.gcount();
  if (read < PAGE_SIZE) {
    // Reading a page past EOF (freshly allocated) -> zero-fill the remainder.
    std::memset(dst + read, 0, PAGE_SIZE - read);
    io_.clear();
  }
}

void DiskManager::WritePage(page_id_t page_id, const char *src) {
  std::lock_guard<std::mutex> g(latch_);
  size_t offset = static_cast<size_t>(page_id) * PAGE_SIZE;
  io_.seekp(offset);
  io_.write(src, PAGE_SIZE);
  io_.flush();
}

page_id_t DiskManager::AllocatePage() {
  std::lock_guard<std::mutex> g(latch_);
  page_id_t id = num_pages_++;
  // Materialize the page on disk so subsequent reads succeed.
  static char zeros[PAGE_SIZE] = {0};
  size_t offset = static_cast<size_t>(id) * PAGE_SIZE;
  io_.seekp(offset);
  io_.write(zeros, PAGE_SIZE);
  io_.flush();
  return id;
}

void DiskManager::Flush() {
  std::lock_guard<std::mutex> g(latch_);
  io_.flush();
}

}  // namespace minidb
