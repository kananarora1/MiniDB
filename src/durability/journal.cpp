#include "durability/journal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

#include "core/config.h"

namespace heapdb {

namespace {
void putU32(std::string& s, uint32_t v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
void putU64(std::string& s, uint64_t v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof(v));
}
}  // namespace

Journal::Journal(const std::string& path) : path_(path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd_ < 0) throw std::runtime_error("cannot open WAL: " + path);
}

Journal::~Journal() {
  if (fd_ >= 0) ::close(fd_);
}

// Wire format per record: [u8 kind][u64 txn][u32 tableLen][table][u32 blobLen][blob]
void Journal::emit(LogKind k, uint64_t txn, const std::string& table,
                   const std::string& blob) {
  std::string rec;
  rec.push_back(static_cast<char>(k));
  putU64(rec, txn);
  putU32(rec, static_cast<uint32_t>(table.size()));
  rec.append(table);
  putU32(rec, static_cast<uint32_t>(blob.size()));
  rec.append(blob);
  ssize_t n = ::write(fd_, rec.data(), rec.size());
  if (n != static_cast<ssize_t>(rec.size()))
    throw std::runtime_error("WAL write failed");
}

void Journal::noteBegin(uint64_t txn) { emit(LogKind::Begin, txn, "", ""); }

void Journal::noteInsert(uint64_t txn, const std::string& table,
                         const std::string& row) {
  emit(LogKind::Insert, txn, table, row);
}

void Journal::noteErase(uint64_t txn, const std::string& table,
                        const std::string& key) {
  emit(LogKind::Erase, txn, table, key);
}

void Journal::noteCommit(uint64_t txn) {
  emit(LogKind::Commit, txn, "", "");
  // Write-ahead ordering holds because the record is appended before any data
  // page for this txn can be flushed. Whether we additionally force it to the
  // platter now is the durability knob (see kSyncOnCommit).
  if (kSyncOnCommit) flush();
}

void Journal::noteAbort(uint64_t txn) { emit(LogKind::Abort, txn, "", ""); }

void Journal::noteCheckpoint() {
  emit(LogKind::Checkpoint, 0, "", "");
  flush();
}

void Journal::flush() {
  if (fd_ >= 0) ::fsync(fd_);
}

std::vector<LogEntry> Journal::replayAll() {
  std::vector<LogEntry> out;
  int rd = ::open(path_.c_str(), O_RDONLY);
  if (rd < 0) return out;

  // Slurp the whole log; WALs in this mini system stay small.
  std::string all;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(rd, buf, sizeof(buf))) > 0) all.append(buf, n);
  ::close(rd);

  std::size_t off = 0;
  auto need = [&](std::size_t bytes) { return off + bytes <= all.size(); };
  while (need(1)) {
    LogEntry e;
    e.kind = static_cast<LogKind>(static_cast<uint8_t>(all[off]));
    off += 1;
    if (!need(8)) break;
    std::memcpy(&e.txn, all.data() + off, 8);
    off += 8;
    uint32_t tlen = 0;
    if (!need(4)) break;
    std::memcpy(&tlen, all.data() + off, 4);
    off += 4;
    if (!need(tlen)) break;
    e.table.assign(all.data() + off, tlen);
    off += tlen;
    uint32_t blen = 0;
    if (!need(4)) break;
    std::memcpy(&blen, all.data() + off, 4);
    off += 4;
    if (!need(blen)) break;
    e.blob.assign(all.data() + off, blen);
    off += blen;
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace heapdb
