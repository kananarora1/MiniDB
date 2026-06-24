// Track B benchmark: MVCC vs strict 2PL on an in-memory versioned key/value
// store. In-memory on purpose, so we measure the concurrency-control cost and
// not disk I/O. Two stores, one workload driver: MVCC readers use a snapshot and
// take no lock; 2PL reads take a shared lock and writes an exclusive one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/lock_manager.h"

using namespace heapdb;
using Clock = std::chrono::steady_clock;

// --------------------------------------------------------------------------
// MVCC store: append-only versions tagged with a commit timestamp. A snapshot
// is just the clock value at begin; a read returns the newest version whose
// commit timestamp is <= that snapshot. Commits validate first-committer-wins.
// --------------------------------------------------------------------------
class MvccStore {
 public:
  explicit MvccStore(int keys) : data_(keys) {}

  struct Txn {
    uint64_t readTs;
    std::map<int, int64_t> writes;
  };

  Txn begin() { return Txn{clock_.load(), {}}; }

  int64_t read(const Txn& t, int key) {
    std::lock_guard<std::mutex> g(mu_);
    const auto& chain = data_[key];
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
      if (it->cts <= t.readTs) return it->val;
    return 0;
  }

  void stage(Txn& t, int key, int64_t v) { t.writes[key] = v; }

  bool commit(Txn& t) {
    std::lock_guard<std::mutex> g(mu_);
    for (const auto& kv : t.writes) {
      const auto& chain = data_[kv.first];
      if (!chain.empty() && chain.back().cts > t.readTs) return false;  // conflict
    }
    uint64_t cts = ++clock_;
    for (const auto& kv : t.writes) data_[kv.first].push_back({kv.second, cts});
    return true;
  }

 private:
  struct Ver { int64_t val; uint64_t cts; };
  std::mutex mu_;
  std::atomic<uint64_t> clock_{1};
  std::vector<std::vector<Ver>> data_;
};

// --------------------------------------------------------------------------
// Strict 2PL store: shared/exclusive locks via LockManager, held until commit.
// --------------------------------------------------------------------------
class TwoPLStore {
 public:
  explicit TwoPLStore(int keys) : data_(keys, 0) {}

  struct Txn {
    uint64_t id;
    std::map<int, int64_t> writes;
    bool dead = false;
  };

  Txn begin() { return Txn{next_.fetch_add(1), {}, false}; }

  int64_t read(Txn& t, int key) {
    if (!lm_.acquire(t.id, std::to_string(key), LockKind::Shared)) {
      t.dead = true;
      return 0;
    }
    std::lock_guard<std::mutex> g(mu_);
    return data_[key];
  }

  void stage(Txn& t, int key, int64_t v) {
    if (!lm_.acquire(t.id, std::to_string(key), LockKind::Exclusive)) {
      t.dead = true;
      return;
    }
    t.writes[key] = v;
  }

  bool commit(Txn& t) {
    if (t.dead) {
      lm_.releaseAll(t.id);
      return false;
    }
    {
      std::lock_guard<std::mutex> g(mu_);
      for (const auto& kv : t.writes) data_[kv.first] = kv.second;
    }
    lm_.releaseAll(t.id);
    return true;
  }

  void abort(Txn& t) { lm_.releaseAll(t.id); }

 private:
  std::atomic<uint64_t> next_{1};
  std::mutex mu_;
  std::vector<int64_t> data_;
  LockManager lm_;
};

struct Result {
  uint64_t committed = 0;
  uint64_t aborts = 0;
  double seconds = 0;
};

// Run a read-heavy workload: `readRatio` of transactions are read-only (R keys),
// the rest are read-modify-write transactions touching W keys.
template <class Store>
Result drive(Store& store, int threads, int perThread, int keys, double readRatio,
             int R, int W) {
  std::atomic<uint64_t> committed{0}, aborts{0};
  auto worker = [&](int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pick(0, keys - 1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    for (int i = 0; i < perThread; ++i) {
      bool readOnly = coin(rng) < readRatio;
      while (true) {
        auto t = store.begin();
        int64_t acc = 0;
        for (int r = 0; r < R; ++r) acc += store.read(t, pick(rng));
        if (!readOnly)
          for (int w = 0; w < W; ++w) store.stage(t, pick(rng), acc + w);
        if (store.commit(t)) {
          committed.fetch_add(1);
          break;
        }
        aborts.fetch_add(1);  // retry until it sticks
      }
    }
  };
  auto t0 = Clock::now();
  std::vector<std::thread> pool;
  for (int i = 0; i < threads; ++i) pool.emplace_back(worker, i + 1);
  for (auto& th : pool) th.join();
  auto t1 = Clock::now();
  Result res;
  res.committed = committed.load();
  res.aborts = aborts.load();
  res.seconds = std::chrono::duration<double>(t1 - t0).count();
  return res;
}

void report(const std::string& name, const Result& r) {
  double tput = r.committed / r.seconds;
  std::cout << "  " << name << ":  " << r.committed << " txns in " << r.seconds
            << " s  =>  " << static_cast<uint64_t>(tput) << " txn/s"
            << "   (retries: " << r.aborts << ")\n";
}

// A deliberate two-transaction deadlock to show the detector firing.
void deadlockDemo() {
  std::cout << "\n[Deadlock demo - strict 2PL]\n";
  LockManager lm;
  std::atomic<int> victims{0};
  std::mutex step;
  std::condition_variable cv;
  int phase = 0;

  auto t1 = std::thread([&] {
    lm.acquire(1, "A", LockKind::Exclusive);
    {
      std::unique_lock<std::mutex> lk(step);
      phase++;
      cv.notify_all();
      cv.wait(lk, [&] { return phase >= 2; });
    }
    if (!lm.acquire(1, "B", LockKind::Exclusive)) victims++;  // would close cycle
    lm.releaseAll(1);
  });
  auto t2 = std::thread([&] {
    lm.acquire(2, "B", LockKind::Exclusive);
    {
      std::unique_lock<std::mutex> lk(step);
      phase++;
      cv.notify_all();
      cv.wait(lk, [&] { return phase >= 2; });
    }
    if (!lm.acquire(2, "A", LockKind::Exclusive)) victims++;
    lm.releaseAll(2);
  });
  t1.join();
  t2.join();
  std::cout << "  T1 holds A, T2 holds B, each then wants the other's lock.\n";
  std::cout << "  deadlock victims chosen (aborted): " << victims.load()
            << "  (expected 1)\n";
}

int main(int argc, char** argv) {
  int threads = argc > 1 ? std::atoi(argv[1]) : 8;
  int perThread = argc > 2 ? std::atoi(argv[2]) : 20000;
  int keys = 200;

  std::cout << "MiniDB Track B benchmark - MVCC vs strict 2PL\n";
  std::cout << "config: threads=" << threads << ", txns/thread=" << perThread
            << ", keys=" << keys << "\n";

  struct Scenario { const char* name; double readRatio; int R; int W; };
  std::vector<Scenario> scenarios = {
      {"read-heavy (95% read-only, 5% writers)", 0.95, 8, 2},
      {"balanced  (50% read-only, 50% writers)", 0.50, 4, 3},
      {"write-hot (10% read-only, 90% writers)", 0.10, 2, 4},
  };

  for (const Scenario& s : scenarios) {
    std::cout << "\n[" << s.name << "]\n";
    MvccStore mv(keys);
    Result rm = drive(mv, threads, perThread, keys, s.readRatio, s.R, s.W);
    report("MVCC ", rm);
    TwoPLStore tp(keys);
    Result rt = drive(tp, threads, perThread, keys, s.readRatio, s.R, s.W);
    report("2PL  ", rt);
    double speedup = (rt.seconds > 0) ? (rt.seconds / rm.seconds) : 0;
    std::cout << "  -> MVCC speedup: " << speedup << "x\n";
  }

  deadlockDemo();
  return 0;
}
