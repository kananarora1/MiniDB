#include "concurrency/lock_manager.h"

#include <algorithm>

namespace heapdb {

bool LockManager::compatible(const std::vector<Holder>& held, uint64_t txn,
                             LockKind want) const {
  for (const Holder& h : held) {
    if (h.txn == txn) continue;  // we already hold something on this key
    // Anything other than (shared vs shared) conflicts.
    if (h.kind == LockKind::Exclusive || want == LockKind::Exclusive) return false;
  }
  return true;
}

// DFS over the waits-for graph to see if `from` can reach `to` (i.e. adding the
// edge from->to would close a cycle).
bool LockManager::reachable(uint64_t from, uint64_t to,
                            std::unordered_set<uint64_t>& seen) const {
  if (from == to) return true;
  if (!seen.insert(from).second) return false;
  auto it = waitsFor_.find(from);
  if (it == waitsFor_.end()) return false;
  for (uint64_t nxt : it->second)
    if (reachable(nxt, to, seen)) return true;
  return false;
}

bool LockManager::acquire(uint64_t txn, const std::string& key, LockKind kind) {
  std::unique_lock<std::mutex> lk(mu_);
  while (true) {
    std::vector<Holder>& held = held_[key];
    if (compatible(held, txn, kind)) {
      // Upgrade in place if we already hold a weaker lock here.
      bool have = false;
      for (Holder& h : held)
        if (h.txn == txn) {
          if (kind == LockKind::Exclusive) h.kind = LockKind::Exclusive;
          have = true;
        }
      if (!have) held.push_back({txn, kind});
      waitsFor_.erase(txn);
      return true;
    }

    // We must wait. Record edges to every conflicting holder, then look for a
    // cycle through us.
    auto& edges = waitsFor_[txn];
    for (const Holder& h : held)
      if (h.txn != txn) edges.insert(h.txn);
    for (uint64_t blocker : edges) {
      std::unordered_set<uint64_t> seen;
      if (reachable(blocker, txn, seen)) {
        waitsFor_.erase(txn);  // back out; we are the victim
        return false;
      }
    }
    cv_.wait(lk);
  }
}

void LockManager::releaseAll(uint64_t txn) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& kv : held_) {
    auto& v = kv.second;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const Holder& h) { return h.txn == txn; }),
            v.end());
  }
  waitsFor_.erase(txn);
  for (auto& kv : waitsFor_) kv.second.erase(txn);
  cv_.notify_all();
}

}  // namespace heapdb
