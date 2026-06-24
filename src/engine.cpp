#include "engine.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace heapdb {

Engine::Engine(const std::string& dir)
    : dir_(dir),
      disk_(dir + "/heap.dat"),
      cache_(disk_, kDefaultFrames),
      catalog_(dir + "/catalog.meta"),
      wal_(dir + "/wal.log") {
  catalog_.load();
  recover();
}

Engine::~Engine() {
  try {
    checkpoint();
  } catch (...) {
    // best-effort on shutdown
  }
}

// --------------------------------------------------------------------------
// Table open + index rebuild
// --------------------------------------------------------------------------
Engine::OpenTable& Engine::open(const std::string& table) {
  auto it = open_.find(table);
  if (it != open_.end()) return it->second;
  OpenTable t;
  t.meta = &catalog_.at(table);
  t.heap = std::make_unique<TableHeap>(cache_, t.meta->pages, t.meta->layout);
  t.index = std::make_unique<BPlusTree>();
  auto [pos, _] = open_.emplace(table, std::move(t));
  rebuildIndex(pos->second);
  return pos->second;
}

void Engine::rebuildIndex(OpenTable& t) {
  int keyPos = t.meta->layout.keyPos;
  std::unordered_map<std::string, uint64_t> best;
  t.heap->forEach([&](const RowVersion& v) {
    const Cell& key = v.fields[keyPos];
    std::string ek = codec::encodeCell(key);
    auto it = best.find(ek);
    if (it == best.end() || v.born >= it->second) {
      best[ek] = v.born;
      t.index->upsert(key, v.self);
    }
  });
}

// --------------------------------------------------------------------------
// Transaction control
// --------------------------------------------------------------------------
TxnHandle Engine::begin() {
  Snapshot s = registry_.begin();
  TxnHandle h;
  h.id = s.me;
  h.iso = defaultIso_;
  h.snap = s;
  std::lock_guard<std::mutex> g(core_);
  liveSnaps_[s.me] = s;
  wal_.noteBegin(s.me);
  return h;
}

void Engine::commit(const TxnHandle& h) {
  registry_.seal(h.id);     // mark committed before releasing locks
  lock_.releaseAll(h.id);   // 2PL: hand the rows to whoever is waiting
  std::lock_guard<std::mutex> g(core_);
  wal_.noteCommit(h.id);
  liveSnaps_.erase(h.id);
}

void Engine::abort(const TxnHandle& h) {
  registry_.roll(h.id);
  lock_.releaseAll(h.id);
  std::lock_guard<std::mutex> g(core_);
  wal_.noteAbort(h.id);
  liveSnaps_.erase(h.id);
}

// --------------------------------------------------------------------------
// 2PL lock naming / acquisition
// --------------------------------------------------------------------------
std::string Engine::rowLock(const std::string& table, const Cell& key) const {
  return table + "/" + codec::encodeCell(key);
}
std::string Engine::tableLock(const std::string& table) const {
  return table + "/*";
}
void Engine::grab(uint64_t txn, const std::string& name, LockKind kind) {
  if (!lock_.acquire(txn, name, kind))
    throw DeadlockAbort("deadlock detected; transaction chosen as victim");
}

// --------------------------------------------------------------------------
// Visibility helpers (core latch held)
// --------------------------------------------------------------------------
std::optional<Sighting> Engine::mvccVisibleLocked(OpenTable& t, const Cell& key,
                                                  const Snapshot& snap) {
  std::optional<Rid> head = t.index->find(key);
  Rid cur = head ? *head : noRid();
  RowVersion v;
  while (cur.live() && t.heap->load(cur, v)) {
    if (registry_.perceives(v.born, v.dead, snap)) return Sighting{v.fields, cur};
    cur = v.older;
  }
  return std::nullopt;
}

// Under strict 2PL the caller already holds the row lock, so the newest version
// whose creator has committed (or is us) is the current state.
std::optional<Sighting> Engine::latestCommittedLocked(OpenTable& t, const Cell& key,
                                                      uint64_t txn) {
  std::optional<Rid> head = t.index->find(key);
  Rid cur = head ? *head : noRid();
  RowVersion v;
  while (cur.live() && t.heap->load(cur, v)) {
    bool bornOk = (v.born == txn) || registry_.phaseOf(v.born) == TxnPhase::Sealed;
    if (bornOk) {
      bool deadHides = v.dead != kNoTxn &&
                       (v.dead == txn || registry_.phaseOf(v.dead) == TxnPhase::Sealed);
      return deadHides ? std::nullopt : std::optional<Sighting>(Sighting{v.fields, cur});
    }
    cur = v.older;  // skip aborted/uncommitted versions
  }
  return std::nullopt;
}

// --------------------------------------------------------------------------
// Reads
// --------------------------------------------------------------------------
void Engine::scanVisible(const std::string& table, const TxnHandle& h,
                         const std::function<void(const Record&, Rid)>& fn) {
  if (h.iso == Iso::TwoPL) grab(h.id, tableLock(table), LockKind::Shared);
  std::lock_guard<std::mutex> g(core_);
  OpenTable& t = open(table);
  if (h.iso == Iso::Mvcc) {
    t.heap->forEach([&](const RowVersion& v) {
      if (registry_.perceives(v.born, v.dead, h.snap)) fn(v.fields, v.self);
    });
  } else {
    // One visible row per key: walk each chain to its current committed version.
    t.index->range(std::nullopt, std::nullopt, [&](const Cell& key, Rid) {
      auto s = latestCommittedLocked(t, key, h.id);
      if (s) fn(s->row, s->at);
    });
  }
}

std::optional<Sighting> Engine::lookupByKey(const std::string& table,
                                            const Cell& key, const TxnHandle& h) {
  if (h.iso == Iso::TwoPL) grab(h.id, rowLock(table, key), LockKind::Shared);
  std::lock_guard<std::mutex> g(core_);
  OpenTable& t = open(table);
  return h.iso == Iso::Mvcc ? mvccVisibleLocked(t, key, h.snap)
                            : latestCommittedLocked(t, key, h.id);
}

// --------------------------------------------------------------------------
// Writes
// --------------------------------------------------------------------------
void Engine::insertRow(const std::string& table, const Record& row,
                       const TxnHandle& h) {
  int keyPos = catalog_.at(table).layout.keyPos;
  const Cell& key = row[keyPos];
  if (h.iso == Iso::TwoPL) grab(h.id, rowLock(table, key), LockKind::Exclusive);

  std::lock_guard<std::mutex> g(core_);
  OpenTable& t = open(table);
  auto existing = (h.iso == Iso::Mvcc) ? mvccVisibleLocked(t, key, h.snap)
                                       : latestCommittedLocked(t, key, h.id);
  if (existing) throw std::runtime_error("duplicate primary key: " + key.show());

  std::optional<Rid> head = t.index->find(key);
  Rid rid = t.heap->place(row, h.id, head ? *head : noRid());
  t.index->upsert(key, rid);
  wal_.noteInsert(h.id, table, codec::encode(row, t.meta->layout));
}

void Engine::updateRow(const std::string& table, Rid target, const Cell& key,
                       const Record& next, const TxnHandle& h) {
  if (h.iso == Iso::TwoPL) grab(h.id, rowLock(table, key), LockKind::Exclusive);

  std::lock_guard<std::mutex> g(core_);
  OpenTable& t = open(table);
  RowVersion cur;
  if (!t.heap->load(target, cur)) throw std::runtime_error("update: row is gone");

  // First-writer-wins only matters under MVCC; under 2PL the X lock we hold has
  // already serialized writers, so there is nothing to detect.
  if (h.iso == Iso::Mvcc && cur.dead != kNoTxn && cur.dead != h.id &&
      registry_.phaseOf(cur.dead) != TxnPhase::Rolled)
    throw WriteConflict("update conflicts with a concurrent transaction");

  const Cell& newKey = next[t.meta->layout.keyPos];
  t.heap->stamp(target, h.id);
  Rid rid = t.heap->place(next, h.id, target);
  t.index->upsert(newKey, rid);
  wal_.noteErase(h.id, table, codec::encodeCell(key));
  wal_.noteInsert(h.id, table, codec::encode(next, t.meta->layout));
}

void Engine::deleteRow(const std::string& table, Rid target, const Cell& key,
                       const TxnHandle& h) {
  if (h.iso == Iso::TwoPL) grab(h.id, rowLock(table, key), LockKind::Exclusive);

  std::lock_guard<std::mutex> g(core_);
  OpenTable& t = open(table);
  RowVersion cur;
  if (!t.heap->load(target, cur)) throw std::runtime_error("delete: row is gone");
  if (h.iso == Iso::Mvcc && cur.dead != kNoTxn && cur.dead != h.id &&
      registry_.phaseOf(cur.dead) != TxnPhase::Rolled)
    throw WriteConflict("delete conflicts with a concurrent transaction");

  t.heap->stamp(target, h.id);
  wal_.noteErase(h.id, table, codec::encodeCell(key));
}

// --------------------------------------------------------------------------
// DDL + introspection
// --------------------------------------------------------------------------
void Engine::createTable(const CreateStmt& stmt) {
  if (stmt.keyPos < 0)
    throw std::runtime_error("CREATE TABLE requires a PRIMARY KEY in MiniDB");
  RowLayout layout;
  layout.columns = stmt.columns;
  layout.keyPos = stmt.keyPos;
  std::lock_guard<std::mutex> g(core_);
  catalog_.define(stmt.table, layout);
  catalog_.save();
  open(stmt.table);  // make it visible to concurrent readers right away
}

std::size_t Engine::estimatedRows(const std::string& table) {
  std::lock_guard<std::mutex> g(core_);
  return open(table).index->size();
}

bool Engine::hasPrimaryIndex(const std::string& table) {
  return catalog_.at(table).layout.hasKey();
}

void Engine::checkpoint() {
  std::lock_guard<std::mutex> g(core_);
  wal_.noteCheckpoint();
  cache_.flushAll();
  catalog_.save();
}

// --------------------------------------------------------------------------
// Crash recovery (single-threaded, runs in the constructor)
// --------------------------------------------------------------------------
void Engine::recover() {
  std::vector<LogEntry> log = wal_.replayAll();

  std::unordered_set<uint64_t> committed, seen;
  uint64_t maxId = 0;
  for (const LogEntry& e : log) {
    if (e.txn) {
      seen.insert(e.txn);
      maxId = std::max(maxId, e.txn);
    }
    if (e.kind == LogKind::Commit) committed.insert(e.txn);
  }

  for (const std::string& name : catalog_.names()) {
    OpenTable& t = open(name);
    t.heap->forEach([&](const RowVersion& v) {
      maxId = std::max({maxId, v.born, v.dead});
    });
  }

  registry_.openAt(maxId + 1);
  for (uint64_t id : committed) registry_.rememberCommitted(id);
  for (uint64_t id : seen)
    if (!committed.count(id)) registry_.rememberAborted(id);

  for (const LogEntry& e : log) {
    if (e.kind != LogKind::Insert && e.kind != LogKind::Erase) continue;
    if (!committed.count(e.txn)) continue;
    OpenTable& t = open(e.table);
    int keyPos = t.meta->layout.keyPos;

    if (e.kind == LogKind::Insert) {
      Record row = codec::decode(e.blob, t.meta->layout);
      const Cell& key = row[keyPos];
      bool present = false;
      std::optional<Rid> head = t.index->find(key);
      Rid cur = head ? *head : noRid();
      RowVersion v;
      while (cur.live() && t.heap->load(cur, v)) {
        if (v.born == e.txn) { present = true; break; }
        cur = v.older;
      }
      if (!present) {
        Rid rid = t.heap->place(row, e.txn, head ? *head : noRid());
        t.index->upsert(key, rid);
      }
    } else {  // Erase
      Cell key = codec::decodeCell(e.blob);
      std::optional<Rid> head = t.index->find(key);
      Rid cur = head ? *head : noRid();
      RowVersion v;
      while (cur.live() && t.heap->load(cur, v)) {
        if (v.born != e.txn) {
          if (v.dead == kNoTxn) t.heap->stamp(cur, e.txn);
          break;
        }
        cur = v.older;
      }
    }
  }

  if (!log.empty()) {
    cache_.flushAll();
    catalog_.save();
  }
}

}  // namespace heapdb
