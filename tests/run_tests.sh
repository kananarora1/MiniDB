#!/usr/bin/env bash
# MiniDB end-to-end test suite: happy paths, error (sad) paths, durability.
# Drives the compiled `minidb` binary with SQL scripts and asserts on output.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/minidb"
TMP="$(mktemp -d)"
PASS=0
FAIL=0

trap 'rm -rf "$TMP"' EXIT

# run_sql <dir> <sql...>  -> prints engine output
run_sql() {
  local dir="$1"; shift
  printf '%s\n' "$*" | "$BIN" "$dir" /dev/stdin 2>&1
}

want() {  # want <label> <haystack> <needle>
  if printf '%s' "$2" | grep -qF -- "$3"; then
    echo "  ok:   $1"; PASS=$((PASS+1))
  else
    echo "  FAIL: $1 (missing: '$3')"; echo "------"; echo "$2"; echo "------"; FAIL=$((FAIL+1))
  fi
}

deny() {  # deny <label> <haystack> <needle that must be absent>
  if printf '%s' "$2" | grep -qF -- "$3"; then
    echo "  FAIL: $1 (unexpected: '$3')"; FAIL=$((FAIL+1))
  else
    echo "  ok:   $1"; PASS=$((PASS+1))
  fi
}

echo "=== Engine-level MVCC tests ==="
if [ -x "$ROOT/build/test_engine" ]; then
  "$ROOT/build/test_engine" | sed 's/^/  /'
  [ "${PIPESTATUS[0]:-0}" -eq 0 ] || FAIL=$((FAIL+1))
else
  echo "  (build/test_engine not built; skipping - see run_tests for how to build)"
fi

echo "=== Happy path: DDL + CRUD ==="
D="$TMP/h1"
OUT=$(run_sql "$D" "CREATE TABLE u (id INT PRIMARY KEY, name TEXT);
INSERT INTO u VALUES (1,'a'),(2,'b'),(3,'c');
SELECT * FROM u;")
want "create+insert+select" "$OUT" "(3 rows)"
want "row present" "$OUT" "| 2  | b"

echo "=== Happy path: WHERE filter ==="
OUT=$(run_sql "$TMP/h2" "CREATE TABLE n (id INT PRIMARY KEY, v INT);
INSERT INTO n VALUES (1,10),(2,20),(3,30);
SELECT id FROM n WHERE v > 15;")
want "filter keeps matches" "$OUT" "(2 rows)"

echo "=== Happy path: index seek chosen by optimizer ==="
OUT=$(run_sql "$TMP/h3" "CREATE TABLE k (id INT PRIMARY KEY, v INT);
INSERT INTO k VALUES (1,1),(2,2),(3,3);
EXPLAIN SELECT * FROM k WHERE id = 2;")
want "optimizer picks IndexSeek" "$OUT" "IndexSeek"

echo "=== Happy path: UPDATE + DELETE ==="
OUT=$(run_sql "$TMP/h4" "CREATE TABLE p (id INT PRIMARY KEY, v INT);
INSERT INTO p VALUES (1,1),(2,2);
UPDATE p SET v = 99 WHERE id = 1;
SELECT v FROM p WHERE id = 1;
DELETE FROM p WHERE id = 2;
SELECT * FROM p;")
want "update applied" "$OUT" "| 99 |"
want "delete left one row" "$OUT" "(1 row)"

echo "=== Happy path: JOIN ==="
OUT=$(run_sql "$TMP/h5" "CREATE TABLE a (id INT PRIMARY KEY, nm TEXT);
CREATE TABLE b (bid INT PRIMARY KEY, aid INT, it TEXT);
INSERT INTO a VALUES (1,'x'),(2,'y');
INSERT INTO b VALUES (10,1,'p'),(11,2,'q'),(12,1,'r');
SELECT a.nm, b.it FROM a JOIN b ON a.id = b.aid;")
want "join produces 3 rows" "$OUT" "(3 rows)"

echo "=== Happy path: explicit txn COMMIT persists, ROLLBACK discards ==="
D="$TMP/h6"
run_sql "$D" "CREATE TABLE t (id INT PRIMARY KEY, v INT);" >/dev/null
OUT=$(run_sql "$D" "BEGIN; INSERT INTO t VALUES (1,1); COMMIT;
BEGIN; INSERT INTO t VALUES (2,2); ROLLBACK;
SELECT * FROM t;")
want "committed row kept" "$OUT" "| 1  | 1"
deny "rolled-back row discarded" "$OUT" "| 2  | 2"

echo "=== Durability: committed data survives a crash ==="
D="$TMP/h7"
printf "CREATE TABLE s (id INT PRIMARY KEY, v TEXT);\nINSERT INTO s VALUES (1,'survive');\n.crash\n" | "$BIN" "$D" >/dev/null 2>&1
OUT=$(run_sql "$D" "SELECT * FROM s;")
want "row recovered via WAL redo" "$OUT" "survive"

echo "=== Durability: uncommitted data vanishes after crash ==="
D="$TMP/h8"
printf "CREATE TABLE s (id INT PRIMARY KEY, v TEXT);\nINSERT INTO s VALUES (1,'keep');\nBEGIN;\nINSERT INTO s VALUES (2,'lose');\n.crash\n" | "$BIN" "$D" >/dev/null 2>&1
OUT=$(run_sql "$D" "SELECT * FROM s;")
want "committed kept" "$OUT" "keep"
deny "uncommitted dropped" "$OUT" "lose"

echo "=== Concurrency: selectable 2PL isolation through SQL ==="
OUT=$(run_sql "$TMP/iso1" "CREATE TABLE acct (id INT PRIMARY KEY, bal INT);
INSERT INTO acct VALUES (1,100);
SET isolation = 2pl;
BEGIN;
UPDATE acct SET bal = 250 WHERE id = 1;
COMMIT;
SELECT bal FROM acct WHERE id = 1;")
want "SET isolation = 2pl acknowledged" "$OUT" "SET isolation = 2pl"
want "BEGIN reports 2pl mode" "$OUT" "BEGIN (2pl)"
want "2PL update committed and visible" "$OUT" "| 250 |"

OUT=$(run_sql "$TMP/iso2" "SET isolation = mvcc;
CREATE TABLE k (id INT PRIMARY KEY, v INT);
INSERT INTO k VALUES (1,1);
BEGIN;
SET isolation = 2pl;")
want "cannot change isolation mid-transaction" "$OUT" "cannot change isolation inside a transaction"

echo "=== Sad path: error handling ==="
OUT=$(run_sql "$TMP/s1" "CREATE TABLE d (id INT PRIMARY KEY, v INT);
INSERT INTO d VALUES (1,1);
INSERT INTO d VALUES (1,2);")
want "duplicate primary key rejected" "$OUT" "duplicate primary key"

OUT=$(run_sql "$TMP/s2" "CREATE TABLE d (id INT PRIMARY KEY, v INT);
INSERT INTO d VALUES (1);")
want "column count mismatch rejected" "$OUT" "column count mismatch"

OUT=$(run_sql "$TMP/s3" "SELECT * FROM nope;")
want "unknown table rejected" "$OUT" "no such table"

OUT=$(run_sql "$TMP/s4" "CREATE TABLE d (id INT PRIMARY KEY, v INT);
SELECT bogus FROM d;")
want "unknown column rejected" "$OUT" "unknown column"

OUT=$(run_sql "$TMP/s5" "CREATE TABLE d (id INT, v INT);")
want "missing primary key rejected" "$OUT" "requires a PRIMARY KEY"

OUT=$(run_sql "$TMP/s6" "SELCT * FROM x;")
want "syntax error rejected" "$OUT" "ERROR"

OUT=$(run_sql "$TMP/s7" "COMMIT;")
want "commit with no txn rejected" "$OUT" "no transaction"

echo
echo "=================================="
echo " PASSED: $PASS    FAILED: $FAIL"
echo "=================================="
[ "$FAIL" -eq 0 ]
