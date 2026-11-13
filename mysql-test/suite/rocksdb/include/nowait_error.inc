# Runs the NOWAIT scenarios against $engine. A row locked by another
# transaction must make a NOWAIT locking read fail immediately with
# ER_LOCK_NOWAIT, roll back only the statement, and not count as a lock wait
# timeout.

--echo # Engine: $engine
--eval CREATE TABLE t1 (id INT PRIMARY KEY, k INT, KEY (k)) ENGINE=$engine
INSERT INTO t1 VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5);

--connection con1
BEGIN;
SELECT * FROM t1 WHERE id = 2 FOR UPDATE;

--connection default
--echo # Point lookup on the primary key
SET @start = SYSDATE(6);
--error ER_LOCK_NOWAIT
SELECT * FROM t1 WHERE id = 2 FOR UPDATE NOWAIT;
SELECT TIMESTAMPDIFF(MICROSECOND, @start, SYSDATE(6)) < 2000000 AS did_not_wait;

--echo # Shared lock request
SET @start = SYSDATE(6);
--error ER_LOCK_NOWAIT
SELECT * FROM t1 WHERE id = 2 FOR SHARE NOWAIT;
SELECT TIMESTAMPDIFF(MICROSECOND, @start, SYSDATE(6)) < 2000000 AS did_not_wait;

--echo # Lookup through the secondary key
SET @start = SYSDATE(6);
--error ER_LOCK_NOWAIT
SELECT * FROM t1 WHERE k = 2 FOR UPDATE NOWAIT;
SELECT TIMESTAMPDIFF(MICROSECOND, @start, SYSDATE(6)) < 2000000 AS did_not_wait;

--echo # Full scan reaching the locked row
SET @start = SYSDATE(6);
--error ER_LOCK_NOWAIT
SELECT * FROM t1 FOR UPDATE NOWAIT;
SELECT TIMESTAMPDIFF(MICROSECOND, @start, SYSDATE(6)) < 2000000 AS did_not_wait;

--echo # Rows that are not locked are returned
SELECT * FROM t1 WHERE id = 3 FOR UPDATE NOWAIT;

--echo # Only the failing statement is rolled back
BEGIN;
INSERT INTO t1 VALUES (10, 10);
--error ER_LOCK_NOWAIT
SELECT * FROM t1 WHERE id = 2 FOR UPDATE NOWAIT;
SELECT * FROM t1 WHERE id = 10;
COMMIT;
SELECT * FROM t1 WHERE id = 10;

--connection con1
COMMIT;

--connection default
DROP TABLE t1;
