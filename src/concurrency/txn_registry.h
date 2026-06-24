#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "core/config.h"

namespace heapdb {

enum class TxnPhase : uint8_t { Running, Sealed /*committed*/, Rolled /*aborted*/ };

// A transaction's snapshot: the slice of the world it is allowed to see. Taken
// once at BEGIN and never changed, which is what gives us snapshot isolation.
struct Snapshot {
  uint64_t me = 0;        // this txn's id
  uint64_t edge = 0;      // ids >= edge started after us -> invisible
  std::unordered_set<uint64_t> concurrent;  // ids still running when we began
};

// Hands out txn ids, tracks each txn's phase, and answers the question MVCC
// keeps asking: given my snapshot, can I see a version born in txn A and maybe
// killed in txn B? Nothing on disk - after a crash, recovery replays the WAL and
// calls rememberCommitted/rememberAborted to rebuild every old txn's verdict.
class TxnRegistry {
 public:
  Snapshot begin();
  void seal(uint64_t id);   // commit
  void roll(uint64_t id);   // abort

  TxnPhase phaseOf(uint64_t id);

  // Was `id` committed and already finished before `snap` was taken?
  bool settledBefore(uint64_t id, const Snapshot& snap);

  // Visibility verdict for a version stamped (born, dead) under `snap`.
  bool perceives(uint64_t born, uint64_t dead, const Snapshot& snap);

  // --- recovery seeding ---
  void openAt(uint64_t nextId);
  void rememberCommitted(uint64_t id);
  void rememberAborted(uint64_t id);

  uint64_t peekNext();

 private:
  std::mutex mu_;
  uint64_t next_ = 1;
  uint64_t boot_ = 1;  // ids below this come from a previous run
  std::unordered_map<uint64_t, TxnPhase> phase_;
  std::unordered_set<uint64_t> running_;
};

}  // namespace heapdb
