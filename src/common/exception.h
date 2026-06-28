#pragma once
#include <stdexcept>
#include <string>

namespace minidb {

// All recoverable, user-facing errors flow through DBException so the REPL/engine
// can catch them and report a clean message instead of crashing.
class DBException : public std::runtime_error {
 public:
  explicit DBException(const std::string &msg) : std::runtime_error(msg) {}
};

// Raised by the transaction layer when a txn must be rolled back (e.g. deadlock victim).
class TransactionAbortException : public DBException {
 public:
  explicit TransactionAbortException(const std::string &msg)
      : DBException("Transaction aborted: " + msg) {}
};

}  // namespace minidb
