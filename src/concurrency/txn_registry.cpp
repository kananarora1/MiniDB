#include "concurrency/txn_registry.h"

namespace heapdb {

Snapshot TxnRegistry::begin() {
  std::lock_guard<std::mutex> g(mu_);
  Snapshot s;
  s.me = next_++;
  s.edge = s.me;                 // everything assigned before us is < edge
  s.concurrent = running_;       // copy of who is still in flight
  phase_[s.me] = TxnPhase::Running;
  running_.insert(s.me);
  return s;
}

void TxnRegistry::seal(uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  phase_[id] = TxnPhase::Sealed;
  running_.erase(id);
}

void TxnRegistry::roll(uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  phase_[id] = TxnPhase::Rolled;
  running_.erase(id);
}

TxnPhase TxnRegistry::phaseOf(uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = phase_.find(id);
  if (it != phase_.end()) return it->second;
  // An id from before this run that we never recorded: it must have finished
  // (committed) in a prior era, otherwise recovery would have marked it rolled.
  if (id < boot_) return TxnPhase::Sealed;
  return TxnPhase::Rolled;
}

bool TxnRegistry::settledBefore(uint64_t id, const Snapshot& snap) {
  if (id >= snap.edge) return false;            // started after us
  if (snap.concurrent.count(id)) return false;  // was racing us
  return phaseOf(id) == TxnPhase::Sealed;
}

bool TxnRegistry::perceives(uint64_t born, uint64_t dead, const Snapshot& snap) {
  bool created = (born == snap.me) || settledBefore(born, snap);
  if (!created) return false;
  if (dead == kNoTxn) return true;       // never deleted
  if (dead == snap.me) return false;     // we deleted it ourselves
  if (settledBefore(dead, snap)) return false;  // deleted by a committed past txn
  return true;                           // killer is concurrent/aborted/future
}

void TxnRegistry::openAt(uint64_t nextId) {
  std::lock_guard<std::mutex> g(mu_);
  next_ = nextId;
  boot_ = nextId;
}

void TxnRegistry::rememberCommitted(uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  phase_[id] = TxnPhase::Sealed;
}

void TxnRegistry::rememberAborted(uint64_t id) {
  std::lock_guard<std::mutex> g(mu_);
  phase_[id] = TxnPhase::Rolled;
}

uint64_t TxnRegistry::peekNext() {
  std::lock_guard<std::mutex> g(mu_);
  return next_;
}

}  // namespace heapdb
