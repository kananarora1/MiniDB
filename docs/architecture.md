# MiniDB Architecture (Team HeapHackers)

This document walks through the engine layer by layer and traces two requests
end to end. The companion documents are `design_decisions.md` (why we built it
this way) and `test_plan.md` (how we know it works).

## Layers, bottom to top

1. **`pager/` - bytes on disk.**
   - `DiskStore` treats one file as an array of 4 KiB pages addressed by id, and
     can grow the file by one page at a time.
   - `FrameCache` is the buffer pool: a fixed array of frames, a `page → frame`
     map, and a clock hand for eviction. Callers `pin` a page (loading it on a
     miss), read/write through the frame, then `unpin` it (flagging dirty).
   - `SlottedView` interprets a frame's bytes as a slotted page.
   - `TableHeap` turns a list of pages into a versioned record collection. Each
     record is `[born | dead | older | row-bytes]`.

2. **`btree/` - finding a row by key.** An in-memory B+ tree maps each primary
   key to the `Rid` of the newest version of its row. Built at table-open time
   by scanning the heap.

3. **`catalog/` - what tables exist.** A serialized data dictionary: table name,
   column layout, primary-key position, and the list of pages the table owns.
   Saved on DDL and at checkpoint.

4. **`concurrency/` - who sees what.**
   - `TxnRegistry` issues transaction ids, tracks each transaction's phase, and
     answers visibility queries against a snapshot (the heart of MVCC).
   - `LockManager` is the strict-2PL baseline with waits-for deadlock detection.

5. **`durability/` - surviving a crash.** `Journal` is the append-only WAL.

6. **`sql/` and `plan/` - turning text into row streams.** Lexer → parser → AST
   → cost-based planner → Volcano operator tree.

7. **`engine` + `session` + `shell`.** `Engine` is the core tying storage,
   index, catalog, WAL, and the transaction registry together and owning
   recovery. `Session` holds one client's current transaction and dispatches
   statements. `shell` is the REPL / script runner.

## The version & visibility model

Every row version on the heap carries two transaction stamps and a back-pointer:

```
key=1   HEAD ──older──▶  v2 (born=7, dead=0)        ← newest, live
                          v1 (born=3, dead=7)        ← superseded by txn 7
```

A snapshot is `(me, edge, concurrent)`. A version `(born, dead)` is visible iff:

- it was **created** for us - `born == me`, or `born` committed and finished
  before our snapshot (`born < edge`, not in `concurrent`, phase = sealed); and
- it was **not deleted** as far as we can see - `dead == 0`, or `dead` is us, or
  `dead` is a transaction that has not settled before our snapshot.

So for `key=1` above, a snapshot taken before txn 7 committed sees `v1`; one
taken after sees `v2`. Exactly one version per key is visible to any snapshot.

## Trace: `SELECT name FROM users WHERE id = 2`

1. `session.exec` → `Lexer` → `Parser` builds a `SelectStmt`.
2. `Planner::planSelect` sees an equality on the primary key `id`, so it emits
   `IndexSeek(users, pk=2)` instead of a `SeqScan`, then `Project(name)`.
3. `IndexSeek` calls `Engine::lookupByKey`, which finds the chain head in the B+
   tree and walks `older` pointers until a version is visible to the snapshot.
4. `Project` keeps the `name` column; the session renders the result table.

## Trace: `UPDATE users SET age = 31 WHERE id = 2` (inside a transaction)

1. The planner builds the same single-table source (`IndexSeek` here).
2. For the located row, `session` copies it, applies the `SET`, and calls
   `Engine::updateRow(target, newRow, txn)`.
3. The engine loads `target`; if its `dead` stamp was already set by another
   non-aborted transaction, it raises `WriteConflict` (first-writer-wins).
4. Otherwise it stamps `target.dead = txn`, appends a new version
   `(born=txn, older=target)`, repoints the index head, and writes
   `Erase(key)` + `Insert(newRow)` to the WAL.
5. `COMMIT` seals the transaction in the registry and `fsync`s a `Commit`
   record; the new version is now visible to later snapshots.

## Trace: crash and recovery

`.crash` calls `_Exit` without flushing the buffer pool, so dirty pages are
lost but the WAL (fsync'd at each commit) is intact. On restart
`Engine::recover` replays the WAL, learns which transactions committed, rebuilds
indexes from the heap, marks never-committed transactions aborted (hiding their
leftover versions), and re-applies any committed change whose page was lost.
Committed data returns; uncommitted data does not.
