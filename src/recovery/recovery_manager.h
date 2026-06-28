#pragma once

namespace minidb {

class LogManager;
class Catalog;
class BufferPool;

// Simplified ARIES recovery. Runs on startup:
//   Analysis -> classify committed (winner) vs in-flight (loser) transactions.
//   Redo     -> replay ALL logged heap changes in LSN order (repeating history),
//               guarded by each page's LSN for idempotence.
//   Undo     -> roll back loser transactions in reverse LSN order.
// Indexes are not logged; they are rebuilt from the recovered heaps afterwards.
class RecoveryManager {
 public:
  RecoveryManager(LogManager *log, Catalog *catalog, BufferPool *bpm)
      : log_(log), catalog_(catalog), bpm_(bpm) {}

  // Returns true if any log records were processed (i.e. recovery did work).
  bool Recover();

 private:
  LogManager *log_;
  Catalog *catalog_;
  BufferPool *bpm_;
};

}  // namespace minidb
