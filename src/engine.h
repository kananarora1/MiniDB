#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include "btree/bptree.h"
#include "catalog/catalog.h"
#include "concurrency/lock_manager.h"
#include "concurrency/txn_registry.h"
#include "durability/journal.h"
#include "pager/disk_store.h"
#include "pager/frame_cache.h"
#include "pager/table_heap.h"
#include "sql/ast.h"

namespace heapdb {

// Raised when a writer loses a first-writer-wins race (MVCC): the row it wants
// to change was already changed by a concurrent transaction.
struct WriteConflict : std::runtime_error {
  explicit WriteConflict(const std::string& m) : std::runtime_error(m) {}
};

// Raised when a 2PL lock request would close a cycle in the waits-for graph; the
// requesting transaction is the deadlock victim and must abort.
struct DeadlockAbort : std::runtime_error {
  explicit DeadlockAbort(const std::string& m) : std::runtime_error(m) {}
};

// A live transaction: its id, the isolation it runs under, and (for MVCC) the
// snapshot it took at BEGIN. Passed through the query layer so every read/write
// knows how to behave.
struct TxnHandle {
  uint64_t id = 0;
  Iso iso = Iso::Mvcc;
  Snapshot snap;  // meaningful only under MVCC
};

// A row a query is allowed to see, together with its heap address.
struct Sighting {
  Record row;
  Rid at;
};

// The database core. Owns the data file, buffer pool, catalog, WAL, txn
// registry, and lock manager, and gives the query layer its read/write
// primitives. Each transaction runs under one of two disciplines:
//   MVCC (default): reads use a snapshot, no locks; writes append a version and
//     use first-writer-wins.
//   2PL: reads take shared locks, writes exclusive ones, held to commit, with
//     deadlock detection.
// A coarse latch keeps the storage structures thread-safe; 2PL row locks are
// taken outside that latch so blocking and deadlock are real.
class Engine {
 public:
  explicit Engine(const std::string& dir);
  ~Engine();

  // --- DDL ---
  void createTable(const CreateStmt& stmt);

  // --- isolation selection (SET isolation = ...) ---
  void setIsolation(Iso i) { defaultIso_ = i; }
  Iso isolation() const { return defaultIso_; }

  // --- transaction control ---
  TxnHandle begin();
  void commit(const TxnHandle& h);
  void abort(const TxnHandle& h);

  // --- reads ---
  void scanVisible(const std::string& table, const TxnHandle& h,
                   const std::function<void(const Record&, Rid)>& fn);
  std::optional<Sighting> lookupByKey(const std::string& table, const Cell& key,
                                      const TxnHandle& h);

  // --- writes ---
  void insertRow(const std::string& table, const Record& row, const TxnHandle& h);
  void updateRow(const std::string& table, Rid target, const Cell& key,
                 const Record& next, const TxnHandle& h);
  void deleteRow(const std::string& table, Rid target, const Cell& key,
                 const TxnHandle& h);

  // --- introspection for the planner / shell ---
  Catalog& catalog() { return catalog_; }
  TableMeta& meta(const std::string& table) { return catalog_.at(table); }
  std::size_t estimatedRows(const std::string& table);
  bool hasPrimaryIndex(const std::string& table);

  void checkpoint();

 private:
  struct OpenTable {
    TableMeta* meta = nullptr;
    std::unique_ptr<TableHeap> heap;
    std::unique_ptr<BPlusTree> index;  // primary key -> newest version Rid
  };

  OpenTable& open(const std::string& table);
  void rebuildIndex(OpenTable& t);
  void recover();

  // Visibility helpers (assume the core latch is held, take no row locks).
  std::optional<Sighting> mvccVisibleLocked(OpenTable& t, const Cell& key,
                                            const Snapshot& snap);
  std::optional<Sighting> latestCommittedLocked(OpenTable& t, const Cell& key,
                                                uint64_t txn);

  // 2PL helpers.
  std::string rowLock(const std::string& table, const Cell& key) const;
  std::string tableLock(const std::string& table) const;
  void grab(uint64_t txn, const std::string& name, LockKind kind);

  std::string dir_;
  DiskStore disk_;
  FrameCache cache_;
  Catalog catalog_;
  Journal wal_;
  TxnRegistry registry_;
  LockManager lock_;
  std::mutex core_;  // guards the storage structures (buffer pool/heap/index/catalog)
  Iso defaultIso_ = Iso::Mvcc;
  std::map<std::string, OpenTable> open_;
  std::map<uint64_t, Snapshot> liveSnaps_;  // active MVCC txn -> its snapshot
};

}  // namespace heapdb
