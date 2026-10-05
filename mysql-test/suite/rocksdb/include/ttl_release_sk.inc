#
# TTL on a table with secondary indexes, checked with real time only.
# Rows stamped two hours ago are expired (ttl_duration is one hour); rows stamped "now" live.
# Every index must hide the same rows, keep them until compaction, and lose them together.
#
# Parameters:
#   $coltype  type of the ttl_col column
#   $past     expression for a timestamp two hours ago
#   $now      expression for the current time
#

eval CREATE TABLE t1 (
  a INT NOT NULL,
  b INT NOT NULL,
  c VARCHAR(16) NOT NULL,
  u INT NOT NULL,
  ts $coltype NOT NULL,
  PRIMARY KEY (a),
  KEY kb (b),
  KEY kcb (c, b),
  UNIQUE KEY ku (u),
  KEY kts (ts)
) ENGINE=ROCKSDB COMMENT='ttl_duration=3600;ttl_col=ts;';

eval INSERT INTO t1 VALUES (1, 10, 'x1', 100, $past), (2, 20, 'x2', 200, $past),
                           (3, 30, 'x3', 300, $past), (4, 40, 'x4', 400, $past),
                           (5, 50, 'x5', 500, $past);
eval INSERT INTO t1 VALUES (6, 60, 'x6', 600, $now), (7, 70, 'x7', 700, $now),
                           (8, 80, 'x8', 800, $now), (9, 90, 'x9', 900, $now),
                           (10, 100, 'x10', 1000, $now);

--echo # Every index hides the expired rows, including index-only reads
SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY);
SELECT COUNT(*) FROM t1 FORCE INDEX (kb);
SELECT COUNT(*) FROM t1 FORCE INDEX (kcb);
SELECT COUNT(*) FROM t1 FORCE INDEX (ku);
SELECT COUNT(*) FROM t1 FORCE INDEX (kts);
SELECT b FROM t1 FORCE INDEX (kb) WHERE b BETWEEN 10 AND 70 ORDER BY b;
SELECT c, b FROM t1 FORCE INDEX (kcb) WHERE c >= 'x' ORDER BY c, b;
SELECT b FROM t1 FORCE INDEX (kb) WHERE b = 30;
SELECT a, u FROM t1 FORCE INDEX (ku) WHERE u = 300;
SELECT a, b FROM t1 FORCE INDEX (kb) WHERE b IN (20, 60) ORDER BY b;

--echo # A new row may reuse a unique value held by an expired, not yet compacted row
eval INSERT INTO t1 VALUES (11, 110, 'x11', 300, $now);
SELECT a, u FROM t1 FORCE INDEX (ku) WHERE u = 300;

--echo # Without read filtering, every index still stores the expired rows
SET GLOBAL rocksdb_enable_ttl_read_filtering = 0;
SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY);
SELECT COUNT(*) FROM t1 FORCE INDEX (kb);
SELECT COUNT(*) FROM t1 FORCE INDEX (kcb);
SELECT COUNT(*) FROM t1 FORCE INDEX (kts);
SET GLOBAL rocksdb_enable_ttl_read_filtering = 1;

--echo # Compaction removes the expired rows from every index at once
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_compact_cf = 'default';
SET GLOBAL rocksdb_enable_ttl_read_filtering = 0;
SELECT a FROM t1 FORCE INDEX (PRIMARY) ORDER BY a;
SELECT b FROM t1 FORCE INDEX (kb) ORDER BY b;
SELECT c, b FROM t1 FORCE INDEX (kcb) ORDER BY c, b;
SELECT u FROM t1 FORCE INDEX (ku) ORDER BY u;
SELECT COUNT(*) FROM t1 FORCE INDEX (kts);
SET GLOBAL rocksdb_enable_ttl_read_filtering = 1;
CHECK TABLE t1;

DROP TABLE t1;
