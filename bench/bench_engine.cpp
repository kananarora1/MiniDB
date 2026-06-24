// Same read-modify-write workload as bench_mvcc, but driven through the real
// engine (storage + buffer pool + WAL + index) under MVCC vs 2PL. This is what
// the SET isolation feature makes possible.
//
// Heads up: the engine guards its structures with a coarse latch, so the raw
// rate is gated by that latch, not by the concurrency control. Read the ratio,
// not the absolute number.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "engine.h"

using namespace heapdb;
using Clock = std::chrono::steady_clock;

static CreateStmt benchTable() {
  CreateStmt c;
  c.table = "bench";
  c.columns = {{"id", FieldKind::Int64}, {"val", FieldKind::Int64}};
  c.keyPos = 0;
  return c;
}

struct Outcome {
  uint64_t committed = 0;
  uint64_t retries = 0;
  double seconds = 0;
};

// Each transaction reads R distinct keys (in sorted order to keep 2PL mostly
// deadlock-free) and, if it is a writer, increments W of them. Conflicts
// (deadlock under 2PL, first-writer-wins under MVCC) trigger a retry.
Outcome drive(Engine& eng, Iso iso, int threads, int perThread, int keys,
              double readRatio, int R, int W) {
  eng.setIsolation(iso);
  std::atomic<uint64_t> committed{0}, retries{0};

  auto worker = [&](int seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pick(0, keys - 1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    for (int i = 0; i < perThread; ++i) {
      bool readOnly = coin(rng) < readRatio;
      std::vector<int> ks;
      for (int r = 0; r < R; ++r) ks.push_back(pick(rng));
      std::sort(ks.begin(), ks.end());
      ks.erase(std::unique(ks.begin(), ks.end()), ks.end());

      while (true) {
        TxnHandle t = eng.begin();
        try {
          int64_t acc = 0;
          std::vector<Sighting> seen;
          for (int k : ks) {
            auto s = eng.lookupByKey("bench", Cell::ofInt(k), t);
            if (s) { acc += s->row[1].num; seen.push_back(*s); }
          }
          if (!readOnly) {
            int w = 0;
            for (auto& s : seen) {
              if (w++ >= W) break;
              Record next = s.row;
              next[1] = Cell::ofInt(next[1].num + 1);
              eng.updateRow("bench", s.at, s.row[0], next, t);
            }
          }
          eng.commit(t);
          (void)acc;  // reads are for contention, not a result
          committed.fetch_add(1);
          break;
        } catch (const std::exception&) {  // WriteConflict or DeadlockAbort
          eng.abort(t);
          retries.fetch_add(1);
        }
      }
    }
  };

  auto t0 = Clock::now();
  std::vector<std::thread> pool;
  for (int i = 0; i < threads; ++i) pool.emplace_back(worker, i + 100);
  for (auto& th : pool) th.join();
  auto t1 = Clock::now();

  Outcome o;
  o.committed = committed.load();
  o.retries = retries.load();
  o.seconds = std::chrono::duration<double>(t1 - t0).count();
  return o;
}

void report(const char* tag, const Outcome& o) {
  std::printf("  %s: %llu txns in %.3fs => %llu txn/s (retries: %llu)\n", tag,
              (unsigned long long)o.committed, o.seconds,
              (unsigned long long)(o.committed / o.seconds),
              (unsigned long long)o.retries);
}

int main(int argc, char** argv) {
  int threads = argc > 1 ? std::atoi(argv[1]) : 4;
  int perThread = argc > 2 ? std::atoi(argv[2]) : 3000;
  int keys = 100;

  std::printf("MiniDB end-to-end engine benchmark - MVCC vs 2PL (through storage)\n");
  std::printf("config: threads=%d, txns/thread=%d, keys=%d\n", threads, perThread, keys);
  std::printf("note: absolute numbers are gated by the engine's coarse latch;\n"
              "      read the MVCC-vs-2PL *ratio*, not the raw rate.\n");

  struct Mix { const char* name; double ro; int R; int W; };
  std::vector<Mix> mixes = {
      {"read-heavy (90% read-only)", 0.90, 6, 2},
      {"write-hot  (20% read-only)", 0.20, 4, 3},
  };

  for (const Mix& m : mixes) {
    // Fresh database per scenario so seeded state is identical for both modes.
    std::string dir = "/tmp/minidb_bench_engine";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Engine eng(dir);
    eng.createTable(benchTable());
    {
      TxnHandle t = eng.begin();
      for (int k = 0; k < keys; ++k) eng.insertRow("bench", {Cell::ofInt(k), Cell::ofInt(0)}, t);
      eng.commit(t);
    }
    std::printf("\n[%s]\n", m.name);
    Outcome mv = drive(eng, Iso::Mvcc, threads, perThread, keys, m.ro, m.R, m.W);
    report("MVCC", mv);
    Outcome tp = drive(eng, Iso::TwoPL, threads, perThread, keys, m.ro, m.R, m.W);
    report("2PL ", tp);
    std::printf("  -> MVCC/2PL throughput ratio: %.2fx\n",
                (tp.seconds > 0 ? (double)mv.committed / mv.seconds /
                                      ((double)tp.committed / tp.seconds)
                                : 0.0));
  }
  return 0;
}
