# One crash round for wp_prepared_crash_recovery.test.
#
# Recreates t1 with 10 rows, runs a multi-statement MyRocks transaction
# (inserts, updates incl. a unique-key change, deletes), crashes the server at
# debug point $crash_point during COMMIT, restarts it and prints the state.
#
# Parameters:
#   $crash_point  - debug keyword that kills the server during COMMIT
#   $expect_label - text describing the expected outcome

--disable_warnings
DROP TABLE IF EXISTS t1;
--enable_warnings
CREATE TABLE t1 (
  pk INT PRIMARY KEY,
  a INT NOT NULL,
  b VARCHAR(20) NOT NULL,
  UNIQUE KEY ua (a),
  KEY kb (b)
) ENGINE=ROCKSDB;
INSERT INTO t1 VALUES (1,10,'r01'),(2,20,'r02'),(3,30,'r03'),(4,40,'r04'),
                      (5,50,'r05'),(6,60,'r06'),(7,70,'r07'),(8,80,'r08'),
                      (9,90,'r09'),(10,100,'r10');

--echo # Crash point: $crash_point
SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
BEGIN;
INSERT INTO t1 VALUES (11,110,'r11'),(12,120,'r12'),(13,130,'r13');
UPDATE t1 SET b = CONCAT(b, 'x') WHERE pk <= 3;
UPDATE t1 SET a = a + 1000 WHERE pk = 3;
DELETE FROM t1 WHERE pk IN (9, 10);
--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
eval SET SESSION debug = '+d,$crash_point';
--error CR_SERVER_LOST
COMMIT;
--source include/wait_until_disconnected.inc
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--source include/wait_until_connected_again.inc

--let $wp_label= after recovery: $expect_label
--source suite/rocksdb/include/wp_show_state.inc
CHECK TABLE t1;
--echo # Unique keys must match the visible state.
--disable_query_log
--let $new_visible= `SELECT COUNT(*) FROM t1 WHERE pk = 11`
--enable_query_log
if ($new_visible)
{
  --error ER_DUP_ENTRY
  INSERT INTO t1 VALUES (50, 110, 'dup_a');
  INSERT INTO t1 VALUES (9, 90, 'reuse_deleted');
}
if (!$new_visible)
{
  --error ER_DUP_ENTRY
  INSERT INTO t1 VALUES (50, 90, 'dup_a');
  INSERT INTO t1 VALUES (11, 110, 'reuse_rolled_back');
}
SELECT pk, a, b FROM t1 WHERE pk IN (9, 11) ORDER BY pk;
