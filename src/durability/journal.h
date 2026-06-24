#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace heapdb {

// One WAL entry. We log logically (whole rows and keys, not page bytes): short,
// easy to follow, and safe to replay twice since recovery checks the index
// before re-applying.
enum class LogKind : uint8_t {
  Begin = 1,
  Insert = 2,   // payload: table name + encoded row
  Erase = 3,    // payload: table name + encoded key cell
  Commit = 4,
  Abort = 5,
  Checkpoint = 6,
};

struct LogEntry {
  LogKind kind;
  uint64_t txn = 0;
  std::string table;    // for Insert / Erase
  std::string blob;     // encoded row (Insert) or encoded key cell (Erase)
};

// The append-only WAL. A record is always appended before its data page can
// reach disk (write-ahead). Recovery reads the whole log back.
class Journal {
 public:
  explicit Journal(const std::string& path);
  ~Journal();

  void noteBegin(uint64_t txn);
  void noteInsert(uint64_t txn, const std::string& table, const std::string& row);
  void noteErase(uint64_t txn, const std::string& table, const std::string& key);
  void noteCommit(uint64_t txn);   // forces the log to stable storage
  void noteAbort(uint64_t txn);
  void noteCheckpoint();

  void flush();

  // Read the entire log in order (recovery).
  std::vector<LogEntry> replayAll();

 private:
  void emit(LogKind k, uint64_t txn, const std::string& table, const std::string& blob);

  int fd_ = -1;
  std::string path_;
};

}  // namespace heapdb
