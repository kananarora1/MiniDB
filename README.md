# MiniDB - Team HeapHackers

A small but genuinely working relational database engine, built from the ground
up for the Advanced DBMS capstone. It runs SQL over a page-based storage engine
with a B+ tree index, a cost-based planner, snapshot-isolation MVCC, and
write-ahead-logging crash recovery.

- **Extension track:** **Track B - Concurrency (replace 2PL with MVCC).**
- **Language:** C++17, no third-party libraries.
- **Status:** all core features implemented and tested end-to-end; 23 SQL
  integration assertions + 8 engine-level concurrency assertions pass; recovery
  and both MVCC-vs-2PL benchmarks are reproducible from the commands below.
- **Concurrency:** MVCC is the default, and **strict 2PL is a selectable mode**
  (`SET isolation = 2pl`) so transactions can also run under locks with live
  deadlock detection - chosen per session, demonstrable through SQL.

---

## Team

**Team name:** HeapHackers

| Member | Scaler Email | Roll Number |
|--------|--------------|-------------|
| Kanan Arora | kanan.23bcs10180@sst.scaler.com | 10180 |
| Ayush Kesharwani | ayush.23bcs10112@sst.scaler.com | 10112 |
| Samrudh Vandkudri Jain | samrudh.23bcs10123@sst.scaler.com | 10123 |


## 1. Project Overview

**Problem.** Modern databases hide an enormous amount of machinery - paging,
indexing, planning, concurrency, recovery - behind a single `SELECT`. The goal
of this project is to make that machinery concrete by building a relational
engine where every layer is small enough to read in full and explain.

**Goals.**
- Execute real SQL (`CREATE`, `INSERT`, `SELECT` with `WHERE`/`JOIN`, `UPDATE`,
  `DELETE`, `BEGIN`/`COMMIT`/`ROLLBACK`) end to end.
- Store rows durably in paged heap files behind a buffer pool, indexed by a B+
  tree.
- Let a cost-based planner choose between an index seek and a full scan, and
  pick a join build side.
- Run concurrent transactions under **MVCC snapshot isolation** and survive a
  process crash without losing committed data.

**Chosen track - B (Concurrency).** The required transaction model is strict
two-phase locking; the extension replaces it with multi-version concurrency
control so that readers work off a consistent snapshot and never block writers.
We keep a 2PL implementation alongside MVCC purely as the baseline the benchmark
measures against.

---

## 2. System Architecture

```
                          ┌──────────────────────────┐
   SQL text ─────────────▶│  sql/  lexer → parser     │  recursive descent → AST
                          └────────────┬─────────────┘
                                       ▼
                          ┌──────────────────────────┐
                          │  plan/  planner           │  cost-based: scan choice,
                          │         operators         │  join build side, EXPLAIN
                          └────────────┬─────────────┘  Volcano pull operators
                                       ▼
   ┌───────────────────────────────────────────────────────────────────────┐
   │                              engine  (core)                             │
   │   MVCC visibility | first-writer-wins | WAL hooks | crash recovery      │
   └───┬───────────────┬───────────────┬───────────────┬───────────────┬────┘
       ▼               ▼               ▼               ▼               ▼
  concurrency/     btree/          pager/          durability/      catalog/
  txn_registry   bptree (PK     table_heap →      journal (WAL)    data dictionary
  (snapshots,    index, in-mem)  frame_cache →
   visibility)                   disk_store
                                 (clock buffer | slotted pages | file)
```

**Data flow for a query.** Text is tokenised (`sql/lexer`) and parsed into an
AST (`sql/parser`). The planner (`plan/planner`) turns the AST into a tree of
pull operators (`plan/operators`), choosing an index seek or sequential scan per
table and a hash-join build side. Operators call the `Engine`, which reads
**only the row versions visible to the transaction's snapshot** from the
`TableHeap`, which in turn pages data in and out through the `FrameCache` over
the `DiskStore`. Writes append new versions, stamp old ones, and append a record
to the WAL (`durability/journal`).

**Module map.**

| Directory | Responsibility |
|-----------|----------------|
| `core/` | value/`Cell`, schema/`RowLayout`, row `(de)serialization`, `Rid`, constants |
| `pager/` | slotted page format, single-file `DiskStore`, clock-sweep `FrameCache`, version-aware `TableHeap` |
| `btree/` | in-memory B+ tree mapping primary key → newest version address |
| `catalog/` | persistent data dictionary (table shapes + page ownership) |
| `sql/` | lexer, recursive-descent parser, AST |
| `plan/` | cost-based planner + Volcano pull operators |
| `concurrency/` | MVCC `TxnRegistry` (snapshots/visibility), strict-2PL `LockManager` |
| `durability/` | logical write-ahead log |
| `engine.{h,cpp}` | the core that wires it all together and owns recovery |
| `session.{h,cpp}` | one client's transaction state + statement dispatch |
| `shell.cpp` | REPL / script runner |

---

## 3. Storage Layer

**Page format - slotted pages (4 KiB).** Each page has a tiny header
(`slots`, `bodyTop`), a slot directory that grows forward, and record bodies
that grow backward from the end of the page (`pager/slotted_page.h`). A record's
address is `Rid = (page, slot)` and never changes when neighbours are added,
which is exactly what the index needs to point at. Deleting flips a slot to a
tombstone. We chose slotted pages over fixed-length slots because `TEXT` columns
vary in length, and because a stable `Rid` lets us patch a version's delete-stamp
in place without moving its bytes.

**Heap files - `TableHeap`.** A table is a list of pages (tracked in the
catalog). Every physical record carries a 28-byte version header
(`born`, `dead`, `older`) ahead of the row bytes, so one logical row can have a
chain of versions newest-first. Inserts append to the last page with room, or
grow a new page.

**Buffer pool - `FrameCache` (clock-sweep).** A fixed set of frames caches
pages. When all frames are taken, a clock hand sweeps the frames, giving each
recently touched frame one "second chance" (clearing its reference bit) before
evicting the first cold, unpinned frame. Clock-sweep gives close-to-LRU quality
at O(1) per access with no linked list to maintain, and resists a single large
scan flushing the whole pool. Dirty frames are written back on eviction; the
write-ahead rule keeps the WAL ahead of these writes.

---

## 4. Indexing

**Structure - B+ tree (`btree/bptree`).** The primary key index maps a column
value (`Cell`) to the `Rid` of the **newest** version of that row. We use a B+
tree (fan-out 32) rather than a hash index because it keeps keys ordered, so the
same structure serves equality (`id = 7`) and range (`id > 100`) predicates, and
all real entries live in linked leaves so a range scan is a straight walk.

**Node structure.** Internal nodes hold separator keys and child pointers; leaf
nodes hold `(key → Rid)` pairs plus a `next` pointer chaining the leaves left to
right. Inserts split full nodes and copy-up (leaf) or push-up (internal) a
separator; the root grows a new level when it splits.

**Search path.** A lookup descends from the root, taking the child to the right
of any equal separator, down to a leaf, where a binary search finds the key. The
index is built in memory by scanning the heap when a table is opened, so there
is no second on-disk format to keep consistent - the heap remains the single
source of truth.

**Index use under MVCC.** Because a key can have several versions, the index
points at the chain head (newest), and a point lookup walks the `older` chain
until it finds the version visible to the current snapshot.

---

## 5. Query Execution

**Parser.** A hand-written recursive-descent parser (`sql/parser`) turns tokens
into an AST. Each grammar rule is one method, so the code mirrors the language
and is easy to step through. Keywords are matched case-insensitively, so there
is no keyword table.

**Plan generation.** The planner (`plan/planner`) produces a tree of physical
operators and, on `EXPLAIN`, a readable summary of the choices it made.

**Operator execution - Volcano pull model.** Every operator implements
`pull()` returning the next tuple (or nothing). Operators compose uniformly:

- `SeqScan` - surfaces only snapshot-visible versions of a table.
- `IndexSeek` - primary-key equality through the B+ tree (≤ 1 row).
- `Filter` - keeps tuples satisfying all `AND`-ed predicates.
- `HashJoin` - in-memory hash equi-join (build smaller side, probe the other).
- `Project` - narrows tuples to the selected columns.

`UPDATE`/`DELETE` reuse the single-table source (index seek or filtered scan) to
locate target rows, then call the engine's versioned mutators.

---

## 6. Optimizer

The optimizer is cost-based and lives in the planner.

- **Cardinality** of a table is estimated from its index size.
- **Selectivity:** a primary-key equality is treated as selecting ≈ 1 row; other
  predicates are residual filters applied after the access path.
- **Access-path choice:** if a query has an equality predicate on the primary
  key, the planner picks an `IndexSeek` (estimated cost ≈ 1) over a `SeqScan`
  (estimated cost ≈ row count); otherwise it scans. `EXPLAIN` prints both costs.
- **Join ordering / build side:** for a join, the planner estimates both inputs
  and builds the hash table on the **smaller** relation, probing with the larger,
  so the in-memory side stays small.

Example:

```
minidb> EXPLAIN SELECT * FROM users WHERE id = 2;
QUERY PLAN
  IndexSeek users (pk=2, est_cost=1 vs seq=3)
```

---

## 7. Transactions & Concurrency

**Isolation level - snapshot isolation (MVCC).** Each transaction takes a
**snapshot** at `BEGIN` (`concurrency/txn_registry`): its own id, the id horizon
(everything started after us is invisible), and the set of transactions still
running when it began. A row version stamped `(born, dead)` is visible to a
snapshot iff its creator is committed-and-settled-before-the-snapshot (or is us),
and its deleter is not. Readers therefore take **no locks** and never block.

**Writes - first-writer-wins.** To update or delete, a transaction stamps the
visible version's `dead` field with its own id and (for update) appends a new
version. If that version was already superseded by another transaction that did
not abort, the writer loses the race and raises a `WriteConflict`, which aborts
the transaction. Aborts cost nothing to undo: an aborted transaction's versions
are simply never visible (visibility, not physical rollback).

**Two-phase locking - a selectable mode (`SET isolation = 2pl`).** The engine
also runs **strict 2PL**, chosen per session. In 2PL mode a transaction takes a
**shared** lock to read a row and an **exclusive** lock to write it
(`concurrency/lock_manager.cpp`), holding both to commit. Conflicting requests
block on a condition variable; before blocking, the manager adds an edge to a
**waits-for graph** and checks for a cycle - if one exists the requester is the
deadlock victim and aborts (surfaced to SQL as `ERROR: deadlock; transaction
aborted`). 2PL reads see the latest committed version rather than a snapshot.

The engine is made safe for concurrent transactions by a coarse latch over the
storage structures, while **row locks are taken outside that latch** so blocking
and deadlock are real - two threads can run two SQL transactions that genuinely
deadlock and have one victim chosen.

**Concurrent behaviour we demonstrate:** snapshot isolation, read-your-own-writes,
abort invisibility, first-writer-wins conflicts (MVCC), 2PL committed reads, and a
**real two-transaction deadlock resolved through the engine** - all in
`tests/test_engine.cpp`, plus the `SET isolation` path in `tests/run_tests.sh`.

### Trade-off: why MVCC is the *default* (and what each mode costs)

Both disciplines run through the SQL engine; MVCC is the default. We state the
trade-off plainly.

**Why MVCC is the default.** Track B's mandate is *"replace 2PL with MVCC."*
MVCC's defining property - readers take no locks and never block writers - is the
whole point of the extension, so it is the engine's primary mode.

**What 2PL gives you, and costs.** 2PL provides the stronger, fully
**serializable** guarantee (no write-skew), and is the right baseline for the
comparison. Its cost is exactly what the benchmark shows: readers block behind
writers' exclusive locks, and conflicting transactions can deadlock and abort.

**What MVCC gives you, and costs.** Non-blocking reads and higher throughput
under contention, at the price of **snapshot isolation** (weaker than
serializable - it permits write-skew), more aborts/retries under heavy write
contention (optimistic first-writer-wins), and keeping old versions around.

**How this choice scopes the benchmark.** Because both modes run through the same
engine, we can now compare them **end to end** (`bench/bench_engine.cpp`) as well
as in isolation (`bench/bench_mvcc.cpp`). See §10 for both, and for why the
end-to-end gap is smaller than the in-memory one (the engine's coarse latch, not
the concurrency discipline, dominates end-to-end throughput).

---

## 8. Recovery

**WAL design - logical, row-level (`durability/journal`).** The log records
whole rows and keys rather than raw page bytes: `Begin`, `Insert(table, row)`,
`Erase(table, key)`, `Commit`, `Abort`, `Checkpoint`. Logical logging is short
and explainable, and because redo checks the live index before re-applying, the
log is safe to replay more than once.

**Durability rules.** Commit appends a `Commit` record and `fsync`s the WAL
before the transaction is considered durable (write-ahead). Data pages are *not*
forced at commit (no-force): if the process crashes with dirty pages still in
the buffer pool, those committed changes are recovered from the WAL.

**Crash recovery procedure** (`Engine::recover`):
1. Replay the WAL and collect the set of **committed** transaction ids.
2. Open every table and build its index from the heap, learning the highest
   version stamp on disk.
3. Seed the transaction registry: future ids start past everything seen; every
   old transaction is marked committed or - if it never committed - aborted, so
   any uncommitted versions left on disk become invisible.
4. **REDO** committed `Insert`/`Erase` effects idempotently (skip what the index
   already reflects), restoring committed changes whose data pages were lost.
5. Flush and persist the catalog.

We demonstrate this live with the REPL's `.crash` command (which exits without
flushing): committed rows survive, uncommitted rows vanish.

---

## 9. Extension Track - B (MVCC)

**Motivation.** Under strict 2PL a reader must take a shared lock, so it waits
behind any writer holding an exclusive lock on the same row - read throughput
collapses under write contention. MVCC removes reads from the locking path
entirely by letting them read a consistent past snapshot.

**Design.** Versions live in the heap as a newest-first chain
(`born`/`dead`/`older`). Visibility is computed by `TxnRegistry` against the
transaction's snapshot. Writes use first-writer-wins conflict detection. No
locks are taken for reads; writers only contend with other writers of the same
row. (See §7.)

**Results.** On an 8-thread workload over 200 keys, MVCC beats strict 2PL by
**~7.7× (read-heavy)**, **~11× (balanced)**, and **~3.7× (write-hot)** in
committed-transaction throughput, while the deadlock detector correctly aborts
exactly one victim in the cyclic-wait demo. Full numbers and analysis in §10.

---

## 10. Benchmarks

We run **two** complementary MVCC-vs-2PL benchmarks.

### 10a. Isolated comparison - `bench/bench_mvcc.cpp`

**Setup.** An in-memory versioned key/value store with two interchangeable
backends (MVCC snapshot isolation vs strict 2PL via our `LockManager`). Using an
in-memory store strips away disk I/O so the numbers measure the **concurrency-
control discipline alone** - the variable Track B is about. Workload: 8 threads,
20,000 transactions/thread, 200 keys; three read/write mixes. Machine: Apple
Silicon, 8 cores, 16 GB RAM, Apple clang 17, `-O2`. Reproduce with
`make bench_mvcc && ./bench_mvcc 8 20000`.

Note these are *not* end-to-end SQL rates: they exclude parsing, planning, the
buffer pool, and the WAL, so "1.6 M txn/s" is the cost of the locking-vs-versioning
logic, not of running SQL statements.

**Results** (representative run; see `bench/results/mvcc_vs_2pl.txt`):

| Scenario | MVCC txn/s | 2PL txn/s | MVCC speedup | MVCC retries | 2PL retries |
|----------|-----------:|----------:|-------------:|-------------:|------------:|
| read-heavy (95% RO) | ~1.63 M | ~0.21 M | **7.7×** | 70 | 36 |
| balanced (50% RO) | ~1.43 M | ~0.13 M | **11.1×** | 7.9 K | 1.4 K |
| write-hot (10% RO) | ~0.36 M | ~0.10 M | **3.7×** | 12 K | 2.7 K |

**Analysis.**
- **Reads never block under MVCC**, so the read-heavy mix shows the largest raw
  throughput; 2PL readers serialize behind writers' exclusive locks.
- The **balanced** mix shows the biggest *speedup* because that is where 2PL's
  lock waiting and our deadlock backoffs hurt most, while MVCC reads stay free.
- The **honest trade-off** is visible in the retry columns: under write-hot
  contention MVCC aborts (and retries) far more than 2PL, because optimistic
  first-writer-wins validation rejects conflicting writers at commit. MVCC still
  wins on throughput here, but the gap narrows - exactly the expected behaviour.
- MVCC keeps old versions around, costing memory/space (mitigated in a real
  system by a vacuum/GC pass - see Limitations).

### 10b. End-to-end comparison - `bench/bench_engine.cpp`

Because 2PL is now a real engine mode, we can run the **same workload through the
actual MiniDB engine** (storage + buffer pool + WAL + index) in both isolation
modes. Setup: 4 threads, 3,000 transactions/thread, 100 keys, read-modify-write,
keys touched in sorted order. Reproduce with `make bench_engine && ./bench_engine`.

**Results** (representative run; see `bench/results/engine_mvcc_vs_2pl.txt`):

| Scenario | MVCC txn/s | 2PL txn/s | MVCC/2PL ratio |
|----------|-----------:|----------:|---------------:|
| read-heavy (90% RO) | ~54 K | ~50 K | **1.08×** |
| write-hot (20% RO) | ~25 K | ~22 K | **1.17×** |

**Analysis - why the end-to-end gap is much smaller than 10a.** MVCC still wins,
but by ~1.1-1.2× instead of 7-11×. This is the honest and instructive result: our
teaching engine guards its shared structures (buffer pool, heap, index) with a
**single coarse latch**, so even MVCC reads serialize on that latch - the very
non-blocking advantage MVCC is supposed to have is masked by the latch, not by the
concurrency-control discipline. To make MVCC's advantage materialize end to end you
need fine-grained latching (per-page latches, a latch-crabbing B+ tree), which we
deliberately did not build (out of scope). So:
- **10a** isolates and shows the true potential of the discipline (7-11×);
- **10b** shows what actually reaches the application through *this* engine (~1.1×),
  and *why* the difference exists.

Stating both, and explaining the gap, is the point - it is a more honest account
than reporting only the flattering in-memory number.

---

## 11. Limitations

- **Type system** is two types (`INT`, `TEXT`); no `NULL`, `FLOAT`, or dates.
- **Tables must declare a `PRIMARY KEY`**, which keeps logical-WAL recovery
  unambiguous. No secondary indexes.
- **`WHERE` is a conjunction (`AND`) of `column OP literal`**; no `OR`,
  expressions, `GROUP BY`/aggregates, or `ORDER BY`/`LIMIT`.
- **Joins** are single equi-joins (two tables); the planner's join ordering is
  the two-table build-side choice rather than full N-way DP.
- **No version GC/vacuum:** dead versions and aborted rows stay on disk (a space
  leak), and the in-memory index is rebuilt by scanning the heap at open.
- **Recovery** replays the whole WAL (checkpoints flush but do not yet truncate
  the log); catalog DDL durability relies on the catalog file, not the WAL.
- **Coarse engine latch:** the engine is made thread-safe with a single latch
  over its storage structures, so end-to-end throughput does not scale with cores
  and MVCC's non-blocking-read advantage is masked end to end (see §10b). Real
  scaling needs fine-grained latching (per-page latches, a latch-crabbing B+
  tree), which is out of scope.
- **The REPL is single-session**, so a live two-transaction deadlock is shown via
  the multi-threaded `tests/test_engine.cpp` (two threads on one engine) rather
  than two interactive shells; the SQL `SET isolation` path itself is covered in
  `tests/run_tests.sh`.

**Future improvements:** fine-grained latching so MVCC's read concurrency scales
end to end; secondary B+ tree indexes; a vacuum thread for dead versions; log
truncation after checkpoints; `OR`/aggregates/`ORDER BY`; serializable snapshot
isolation (SSI) on top of the current MVCC.

---

## 12. How to Run

**Dependencies:** a C++17 compiler (Apple clang / g++) with pthreads. No other
libraries.

```bash
# from this directory
make              # builds ./minidb, ./bench_mvcc, ./bench_engine
make test         # builds + runs the full suite (SQL + engine concurrency tests)
make bench_mvcc   # in-memory MVCC-vs-2PL benchmark only
make bench_engine # end-to-end (through-the-engine) benchmark only
```

**Interactive shell:**

```bash
./minidb ./data           # data dir is created if missing
```
```sql
CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);
INSERT INTO users VALUES (1,'alice',30),(2,'bob',25);
SELECT name FROM users WHERE age > 26;
EXPLAIN SELECT * FROM users WHERE id = 1;
BEGIN;
UPDATE users SET age = 31 WHERE id = 1;
COMMIT;

-- choose the concurrency-control discipline (default is mvcc):
SET isolation = 2pl;       -- strict two-phase locking, with deadlock detection
BEGIN;                     -- prompt reports: BEGIN (2pl)
UPDATE users SET age = 26 WHERE id = 2;
COMMIT;
SET isolation = mvcc;      -- back to snapshot isolation
```
Meta-commands: `.tables`, `.crash` (simulate a crash - restart to see recovery),
`.exit`.

**Run a script:** `./minidb ./data path/to/script.sql`

**Benchmarks:**
- `./bench_mvcc [threads] [txns_per_thread]` - isolated comparison (defaults `8 20000`).
- `./bench_engine [threads] [txns_per_thread]` - end-to-end through the engine (defaults `4 3000`).

---

## Repository Layout

```
Team_HeapHackers/
├── README.md                ← this file
├── Makefile
├── src/                     ← engine source (see module map in §2)
├── bench/
│   ├── bench_mvcc.cpp        ← isolated MVCC vs 2PL benchmark + deadlock demo
│   ├── bench_engine.cpp      ← end-to-end MVCC vs 2PL through the real engine
│   └── results/
├── tests/
│   ├── run_tests.sh          ← SQL happy/sad/durability + SET isolation asserts
│   └── test_engine.cpp       ← engine MVCC + 2PL deadlock correctness tests
└── docs/
    ├── architecture.md
    ├── design_decisions.md
    └── test_plan.md
```
