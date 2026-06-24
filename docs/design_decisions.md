# MiniDB Design Decisions (Team HeapHackers)

We record decisions as short ADR-style notes: the **context** (the problem), the
**alternatives** we weighed, the **decision**, and the **consequences** (what we
gave up). The guiding principle was: pick the option we can implement correctly
*and* explain in a viva, and prefer the choice real databases make when it costs
us little.

---

### DR-1 - Buffer-pool eviction: clock-sweep

**Context.** With a fixed number of resident frames we need an eviction victim
when the pool is full.
**Alternatives.** FIFO (ignores hotness); exact LRU (must reorder a list on
every access); clock-sweep (approximate LRU, O(1)).
**Decision.** Clock-sweep - each frame has a reference bit; a hand sweeps,
clearing bits and evicting the first cold, unpinned frame.
**Consequences.** We get near-LRU quality with no per-access list surgery and
scan resistance, at the cost of only *approximating* true recency. Implemented
in `pager/frame_cache.cpp`.

---

### DR-2 - On-page layout: slotted pages

**Context.** Rows include variable-length `TEXT`, and the index must hold stable
pointers to rows.
**Alternatives.** Fixed-length slots (trivial addressing, wastes/cannot fit
variable text); slotted pages (a slot directory of `(offset, length)`).
**Decision.** Slotted pages, with the body growing down from the page end and
the directory growing up from the header.
**Consequences.** Variable text fits, `Rid = (page, slot)` stays valid as
neighbours change, and we can patch a version's delete-stamp in place. Cost: a
few bytes of slot overhead per row. Implemented in `pager/slotted_page.h`.

---

### DR-3 - Primary index: in-memory B+ tree

**Context.** We need fast key lookups and the ability to range-scan.
**Alternatives.** Hash index (no ranges, no order); B-tree (data in every node);
B+ tree (data only in linked leaves).
**Decision.** A B+ tree, built in memory by scanning the heap at open time and
mapping each key to its newest version's `Rid`.
**Consequences.** Equality and range predicates share one ordered structure, and
there is no second on-disk format to keep consistent - the heap stays the source
of truth. Cost: a rebuild cost at startup and memory proportional to key count.
We chose **not** to persist the index on disk to keep the storage format simple.

---

### DR-4 - Parser: hand-written recursive descent

**Context.** We need to turn SQL text into an AST for a small language subset.
**Alternatives.** A parser generator (yacc/ANTLR) or a hand-written parser.
**Decision.** Recursive descent - one method per grammar rule.
**Consequences.** Every line is ours and easy to walk through; no generated code
or build-time tool. Cost: we would hand-code precedence if the grammar grew.
Implemented in `sql/parser.cpp`.

---

### DR-5 - Execution model: Volcano pull operators

**Context.** Operators (scan, filter, join, project) must compose.
**Alternatives.** Materialize each step fully; or a pull/`next()` iterator model.
**Decision.** Volcano pull - every operator exposes `pull()` and advertises its
output columns, so parents bind predicate/projection references by position.
**Consequences.** Operators snap together in any order and upper operators stream
rows. We buffer base scans (a scan reads its visible set up front) for
simplicity; making execution *faster* with vectorization is Track A's job, so we
deliberately left it out.

---

### DR-6 - Join algorithm: in-memory hash join

**Context.** Our joins are equi-joins on a foreign-key-like column.
**Alternatives.** Nested-loop (simplest, O(n*m)); sort-merge (needs sorting);
hash join (O(1) probe per row for equi-joins).
**Decision.** Build a hash table on the smaller input and probe with the larger.
**Consequences.** Equi-joins - the common case - run in roughly linear time, and
the planner's "build the smaller side" rule keeps the hash table small. Cost:
equi-joins only, and the build side must fit in memory. Implemented as
`HashJoin` in `plan/operators.cpp`.

---

### DR-7 - Concurrency control: MVCC snapshot isolation (Track B)

**Context.** Strict 2PL makes readers wait behind writers; Track B asks us to
replace it with MVCC and compare.
**Alternatives.** Strict 2PL (strong, but readers block); MVCC snapshot
isolation (readers never block, slightly weaker guarantee); SSI (serializable,
more machinery).
**Decision.** MVCC snapshot isolation as the **default**, with first-writer-wins
write-conflict detection - *and* strict 2PL as a **selectable per-session mode**
(`SET isolation = 2pl`) wired through the same SQL engine, with shared/exclusive
locks and waits-for deadlock detection (`concurrency/lock_manager.*`). The engine
is made concurrency-safe with a coarse latch over its storage structures, while
row locks are taken outside that latch so 2PL blocking and deadlock are real.
**Consequences.** We can demonstrate *both* disciplines through SQL and compare
them isolated (7-11×, `bench_mvcc`) and end to end (~1.1×, `bench_engine`). The
trade-offs we accept and document: (a) MVCC's snapshot isolation is not fully
serializable (write-skew), whereas 2PL is; (b) under heavy write contention MVCC
aborts/retries more than 2PL; (c) old versions accumulate (see DR-9); (d) the
coarse latch caps end-to-end scaling, masking MVCC's read-concurrency win unless
fine-grained latching is added. Implemented in `concurrency/*` and `engine.cpp`.

---

### DR-8 - Recovery: logical, row-level WAL with no-force commit

**Context.** Committed transactions must survive a crash even if their data
pages never reached disk.
**Alternatives.** Physical (page-byte) logging like ARIES (precise, heavy);
logical (whole-row) logging (coarser, simple, idempotent if redo checks first).
**Decision.** Logical logging - `Insert(table,row)`, `Erase(table,key)`,
`Commit`, etc. Commit fsyncs the WAL (write-ahead); data pages are not forced
(no-force), so redo restores lost committed effects.
**Consequences.** Recovery is explainable and replay is idempotent (redo checks
the live index). Cost: coarser than page-level logging, and recovery currently
replays the whole log (checkpoints flush but do not truncate). Implemented in
`durability/journal.cpp` and `Engine::recover`.

---

### DR-9 - Smaller calls (kept deliberately simple)

- **Require a `PRIMARY KEY` per table** - makes logical-WAL erase-by-key
  unambiguous and gives every table an index. We reject keyless `CREATE TABLE`.
- **Abort by visibility, not undo** - an aborted transaction's versions are
  simply never marked committed, so they are invisible; nothing to roll back.
- **No version GC** - dead/aborted versions stay on disk (a space leak). A
  vacuum pass is future work; we document the trade-off rather than hide it.
- **Two column types (`INT`, `TEXT`)** - enough to demonstrate every layer; the
  serialization format and comparison rules stay tiny.
- **`.crash` via `_Exit`** - exits without flushing, a faithful and repeatable
  stand-in for a hard crash in the recovery demo.
