# One scenario of crash_unsafe_alter_partition.
#
# Parameters:
#   $alter       the ALTER TABLE statement (ALGORITHM=INPLACE partition op)
#   $sync_point  DEBUG_SYNC point at which the ALTER is parked when the
#                server is killed

--echo # Scenario: $alter, killed at $sync_point
CREATE TABLE t1 (id INT PRIMARY KEY, v INT) ENGINE=ROCKSDB
  PARTITION BY RANGE (id) (
    PARTITION p0 VALUES LESS THAN (100),
    PARTITION p1 VALUES LESS THAN (200),
    PARTITION p2 VALUES LESS THAN (300));
INSERT INTO t1 VALUES (1, 1), (2, 2), (101, 101), (102, 102), (201, 201),
                      (202, 202);

--connect (con_alter, localhost, root,,)
--eval SET DEBUG_SYNC = '$sync_point SIGNAL parked WAIT_FOR never_happens'
--send_eval $alter

--connection default
SET DEBUG_SYNC = 'now WAIT_FOR parked';
--source include/kill_and_restart_mysqld.inc
--disconnect con_alter
--connection default

--echo # State after recovery (errors are printed, not fatal)
--disable_abort_on_error
SELECT PARTITION_NAME FROM INFORMATION_SCHEMA.PARTITIONS
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'
  ORDER BY PARTITION_ORDINAL_POSITION;
SELECT PARTITION_NAME FROM INFORMATION_SCHEMA.ROCKSDB_DDL
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'
  ORDER BY PARTITION_NAME;
SELECT COUNT(*) FROM t1;
SELECT id FROM t1 ORDER BY id;
INSERT INTO t1 VALUES (50, 50), (150, 150), (250, 250);
SELECT COUNT(*) FROM t1;
CHECK TABLE t1;
--echo # Plain DROP TABLE
DROP TABLE t1;
--echo # Cleanup that always works
DROP TABLE IF EXISTS t1;
--echo # MyRocks dictionary entries left for t1 after the table is gone
SELECT PARTITION_NAME FROM INFORMATION_SCHEMA.ROCKSDB_DDL
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'
  ORDER BY PARTITION_NAME;
--echo # Re-create the table with the same partition names, including p3
CREATE TABLE t1 (id INT PRIMARY KEY, v INT) ENGINE=ROCKSDB
  PARTITION BY RANGE (id) (
    PARTITION p0 VALUES LESS THAN (100),
    PARTITION p1 VALUES LESS THAN (200),
    PARTITION p2 VALUES LESS THAN (300),
    PARTITION p3 VALUES LESS THAN (400));
INSERT INTO t1 VALUES (1, 1), (101, 101), (201, 201), (301, 301);
SELECT COUNT(*) FROM t1;
DROP TABLE IF EXISTS t1;
--enable_abort_on_error
