// Engine-level tests that interleave two transactions in one process: MVCC
// snapshot isolation, read-your-writes, abort visibility, first-writer-wins,
// and - now that the engine supports it - strict 2PL committed reads and a real
// deadlock detected through the engine.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "engine.h"

using namespace heapdb;

static int failures = 0;
#define CHECK(cond, msg)                              \
  do {                                                \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++failures; } \
    else { std::printf("  ok:   %s\n", msg); }        \
  } while (0)

static CreateStmt makeAcct() {
  CreateStmt c;
  c.table = "acct";
  c.columns = {{"id", FieldKind::Int64}, {"bal", FieldKind::Int64}};
  c.keyPos = 0;
  return c;
}
static Record row(int64_t id, int64_t bal) {
  return {Cell::ofInt(id), Cell::ofInt(bal)};
}

int main() {
  std::string dir = "/tmp/minidb_engine_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  Engine eng(dir);
  eng.createTable(makeAcct());
  {
    TxnHandle t = eng.begin();
    eng.insertRow("acct", row(1, 100), t);
    eng.insertRow("acct", row(2, 100), t);
    eng.commit(t);
  }

  std::printf("[MVCC snapshot isolation]\n");
  {
    TxnHandle reader = eng.begin();
    TxnHandle writer = eng.begin();
    eng.insertRow("acct", row(9, 50), writer);
    eng.commit(writer);
    auto seen = eng.lookupByKey("acct", Cell::ofInt(9), reader);
    CHECK(!seen.has_value(), "old snapshot does not see a row committed after it");
    eng.commit(reader);
    TxnHandle fresh = eng.begin();
    CHECK(eng.lookupByKey("acct", Cell::ofInt(9), fresh).has_value(),
          "new snapshot sees the committed row");
    eng.commit(fresh);
  }

  std::printf("[MVCC read-your-own-writes]\n");
  {
    TxnHandle t = eng.begin();
    eng.insertRow("acct", row(3, 7), t);
    auto mine = eng.lookupByKey("acct", Cell::ofInt(3), t);
    CHECK(mine && mine->row[1].num == 7, "txn sees its own uncommitted insert");
    eng.commit(t);
  }

  std::printf("[MVCC abort invisibility]\n");
  {
    TxnHandle t = eng.begin();
    eng.insertRow("acct", row(4, 999), t);
    eng.abort(t);
    TxnHandle chk = eng.begin();
    CHECK(!eng.lookupByKey("acct", Cell::ofInt(4), chk).has_value(),
          "aborted insert is invisible to later txns");
    eng.commit(chk);
  }

  std::printf("[MVCC first-writer-wins]\n");
  {
    TxnHandle t1 = eng.begin(), t2 = eng.begin();
    auto r1 = eng.lookupByKey("acct", Cell::ofInt(1), t1);
    auto r2 = eng.lookupByKey("acct", Cell::ofInt(1), t2);
    eng.updateRow("acct", r1->at, Cell::ofInt(1), row(1, 200), t1);
    eng.commit(t1);
    bool conflicted = false;
    try {
      eng.updateRow("acct", r2->at, Cell::ofInt(1), row(1, 300), t2);
    } catch (const WriteConflict&) { conflicted = true; }
    CHECK(conflicted, "second writer hits a write-write conflict");
    eng.abort(t2);
  }

  // ---- strict 2PL ----
  eng.setIsolation(Iso::TwoPL);

  std::printf("[2PL committed read]\n");
  {
    TxnHandle w = eng.begin();
    auto cur = eng.lookupByKey("acct", Cell::ofInt(2), w);
    eng.updateRow("acct", cur->at, Cell::ofInt(2), row(2, 555), w);
    eng.commit(w);  // releases locks
    TxnHandle r = eng.begin();
    auto got = eng.lookupByKey("acct", Cell::ofInt(2), r);
    CHECK(got && got->row[1].num == 555, "2PL reader sees the latest committed value");
    eng.commit(r);
  }

  std::printf("[2PL deadlock detected through the engine]\n");
  {
    std::atomic<int> ready{0};
    std::atomic<int> victims{0};
    std::atomic<int> winners{0};

    auto worker = [&](int64_t first, int64_t second) {
      TxnHandle t = eng.begin();  // 2PL
      try {
        auto a = eng.lookupByKey("acct", Cell::ofInt(first), t);
        eng.updateRow("acct", a->at, Cell::ofInt(first), row(first, first * 10), t);
        ready.fetch_add(1);
        while (ready.load() < 2) std::this_thread::yield();  // both hold lock #1
        auto b = eng.lookupByKey("acct", Cell::ofInt(second), t);  // crosses -> may deadlock
        eng.updateRow("acct", b->at, Cell::ofInt(second), row(second, second * 10), t);
        eng.commit(t);
        winners.fetch_add(1);
      } catch (const DeadlockAbort&) {
        eng.abort(t);
        victims.fetch_add(1);
      }
    };
    std::thread tA(worker, 1, 2);
    std::thread tB(worker, 2, 1);
    tA.join();
    tB.join();
    CHECK(victims.load() == 1, "exactly one transaction is the deadlock victim");
    CHECK(winners.load() == 1, "the other transaction commits");
  }

  std::printf(failures == 0 ? "\nALL ENGINE TESTS PASSED\n"
                            : "\n%d ENGINE TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
