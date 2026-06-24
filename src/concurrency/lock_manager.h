#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heapdb {

enum class LockKind { Shared, Exclusive };

// Strict-2PL lock manager: shared/exclusive locks held to end of transaction.
// Conflicting requests block on a condition variable. Before a request waits we
// add an edge to a waits-for graph and look for a cycle; if there is one the
// requester is the deadlock victim and is told to abort.
class LockManager {
 public:
  // Returns true if the lock was granted, false if granting it would deadlock
  // (the caller must then abort and release whatever it holds).
  bool acquire(uint64_t txn, const std::string& key, LockKind kind);

  // Release every lock held by `txn` and wake anyone waiting.
  void releaseAll(uint64_t txn);

 private:
  struct Holder {
    uint64_t txn;
    LockKind kind;
  };

  bool compatible(const std::vector<Holder>& held, uint64_t txn, LockKind want) const;
  bool reachable(uint64_t from, uint64_t to,
                 std::unordered_set<uint64_t>& seen) const;

  std::mutex mu_;
  std::condition_variable cv_;
  std::unordered_map<std::string, std::vector<Holder>> held_;
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> waitsFor_;
};

}  // namespace heapdb
