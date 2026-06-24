# MiniDB Test Plan (Team HeapHackers)

Two layers of automated tests, both run by `make test`:

- **SQL integration** - `tests/run_tests.sh` drives the compiled `minidb`
  binary with scripts and asserts on the printed output (happy paths, sad/error
  paths, and durability across a simulated crash).
- **Engine-level MVCC** - `tests/test_engine.cpp` drives the `Engine` API
  directly so two transactions can be interleaved in one process (snapshot
  isolation, read-your-writes, abort visibility, first-writer-wins), which the
  single-session REPL cannot show.

## Happy paths (expected to succeed)

| # | Scenario | Asserts |
|---|----------|---------|
| H1 | `CREATE` + multi-row `INSERT` + `SELECT *` | 3 rows returned, specific row present |
| H2 | `SELECT … WHERE v > 15` | only matching rows kept |
| H3 | `EXPLAIN … WHERE id = 2` | optimizer prints `IndexSeek` |
| H4 | `UPDATE` then `DELETE` | new value visible; row count drops |
| H5 | two-table equi-`JOIN` | correct number of joined rows |
| H6 | `BEGIN…COMMIT` vs `BEGIN…ROLLBACK` | committed row kept, rolled-back row gone |

## Durability (crash via `.crash`, then restart)

| # | Scenario | Asserts |
|---|----------|---------|
| D1 | commit a row, crash before flush, restart | row recovered from WAL |
| D2 | commit one row, start a second uncommitted insert, crash, restart | committed row kept; uncommitted row vanishes |

## Sad paths (expected to error gracefully, not crash)

| # | Scenario | Expected message |
|---|----------|------------------|
| S1 | insert duplicate primary key | `duplicate primary key` |
| S2 | insert with wrong column count | `column count mismatch` |
| S3 | select from unknown table | `no such table` |
| S4 | select an unknown column | `unknown column` |
| S5 | `CREATE TABLE` without a primary key | `requires a PRIMARY KEY` |
| S6 | malformed SQL | `ERROR` (parser rejects) |
| S7 | `COMMIT` with no active transaction | `no transaction` |

## Engine-level MVCC (concurrent transactions)

| # | Scenario | Asserts |
|---|----------|---------|
| M1 | reader's snapshot taken before a writer commits | reader does **not** see the new row; a fresh snapshot does |
| M2 | a transaction reads back its own uncommitted insert | sees its own write |
| M3 | a transaction inserts then aborts | the row is invisible to later transactions |
| M4 | two transactions update the same row | the second to write hits `WriteConflict`; the winner's value stands |

## Performance (not pass/fail; reported)

`bench/bench_mvcc.cpp` measures committed-transaction throughput for MVCC vs
strict 2PL across read-heavy, balanced, and write-hot mixes, and runs a
two-transaction deadlock that the 2PL detector must resolve by aborting exactly
one victim. Captured results live in `bench/results/mvcc_vs_2pl.txt`.

## How to run

```bash
make test          # SQL integration + engine MVCC tests
make bench_mvcc && ./bench_mvcc 8 20000   # performance + deadlock demo
```
