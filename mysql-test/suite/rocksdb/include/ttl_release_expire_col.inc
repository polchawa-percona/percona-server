#
# TTL with an explicit ttl_col, checked with real time only (no debug_ttl_* variables).
# Rows whose ttl_col lies two hours in the past are expired; rows stamped "now" are live
# for an hour, so the test has no timing window.
#
# Parameters:
#   $coltype  type of the ttl_col column
#   $past     expression for a timestamp two hours ago
#   $now      expression for the current time
#

eval CREATE TABLE t1 (
  a INT NOT NULL,
  ts $coltype NOT NULL,
  v VARCHAR(32) NOT NULL,
  PRIMARY KEY (a)
) ENGINE=ROCKSDB COMMENT='ttl_duration=3600;ttl_col=ts;';

eval INSERT INTO t1 VALUES (1, $past, 'old'), (2, $past, 'old'), (3, $past, 'old'),
                           (4, $past, 'old'), (5, $past, 'old');
eval INSERT INTO t1 VALUES (6, $now, 'new'), (7, $now, 'new'), (8, $now, 'new'),
                           (9, $now, 'new'), (10, $now, 'new');

--echo # Expired rows are hidden from scans, point lookups and DML
SELECT a, v FROM t1 ORDER BY a;
SELECT COUNT(*) FROM t1;
SELECT a FROM t1 WHERE a = 1;
SELECT a FROM t1 WHERE a BETWEEN 1 AND 7 ORDER BY a;
UPDATE t1 SET v = 'touched' WHERE a = 2;
SELECT ROW_COUNT();
DELETE FROM t1 WHERE a = 4;
SELECT ROW_COUNT();

--echo # A new row may reuse the primary key of an expired, not yet compacted row
eval INSERT INTO t1 VALUES (3, $now, 'reused');
SELECT a, v FROM t1 WHERE a = 3;

--echo # Without read filtering the expired rows are still stored
SET GLOBAL rocksdb_enable_ttl_read_filtering = 0;
SELECT a, v FROM t1 ORDER BY a;
SET GLOBAL rocksdb_enable_ttl_read_filtering = 1;

--echo # Compaction removes exactly the expired rows and keeps the reused key
SELECT variable_value INTO @expired FROM performance_schema.global_status
  WHERE variable_name = 'rocksdb_rows_expired';
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_compact_cf = 'default';
SELECT variable_value - @expired AS rows_expired FROM performance_schema.global_status
  WHERE variable_name = 'rocksdb_rows_expired';

SET GLOBAL rocksdb_enable_ttl_read_filtering = 0;
SELECT a, v FROM t1 ORDER BY a;
SET GLOBAL rocksdb_enable_ttl_read_filtering = 1;
SELECT a, v FROM t1 ORDER BY a;
CHECK TABLE t1;

DROP TABLE t1;
