-- MiniDB demo (Team HeapHackers, Track B / MVCC)
-- run with:  ./minidb ./demo_data examples/demo.sql

-- two tables, each with a primary-key B+ tree index
CREATE TABLE users  (id INT PRIMARY KEY, name TEXT, age INT);
CREATE TABLE orders (oid INT PRIMARY KEY, uid INT, item TEXT);

-- rows land in slotted heap pages through the buffer pool
INSERT INTO users VALUES (1,'alice',30),(2,'bob',25),(3,'carol',41);
INSERT INTO orders VALUES (100,1,'keyboard'),(101,3,'monitor'),(102,1,'mouse');

-- scan + filter
SELECT name, age FROM users WHERE age > 28;

-- the optimizer takes the index for a primary-key match
EXPLAIN SELECT * FROM users WHERE id = 2;
SELECT * FROM users WHERE id = 2;

-- hash join across the two tables
SELECT users.name, orders.item FROM users JOIN orders ON users.id = orders.uid;

-- update makes a new version; the old one stays but is hidden
UPDATE users SET age = 31 WHERE id = 1;
SELECT id, name, age FROM users WHERE id = 1;

-- delete marks the visible version dead
DELETE FROM orders WHERE oid = 102;
SELECT * FROM orders;

-- rollback throws the insert away
BEGIN;
INSERT INTO users VALUES (4,'dave',50);
ROLLBACK;
SELECT * FROM users;

-- strict 2PL is a switch away
SET isolation = 2pl;
BEGIN;
UPDATE users SET age = 26 WHERE id = 2;
COMMIT;
SELECT id, age FROM users WHERE id = 2;
