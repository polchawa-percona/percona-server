# One scenario of crash_unsafe_alter_partition_recovery.
#
# Parameters:
#   $alter       the ALTER TABLE statement (ALGORITHM=INPLACE partition op)
#   $sync_point  DEBUG_SYNC point at which the ALTER is parked when the
#                server is killed
#
# Every statement after recovery must succeed (abort on error stays on).

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

--echo # The table is usable and the untouched partitions keep their rows
SELECT id FROM t1 WHERE id < 200 ORDER BY id;
INSERT INTO t1 VALUES (50, 50), (150, 150);
SELECT COUNT(*) FROM t1 WHERE id < 200;
CHECK TABLE t1;
--echo # Plain DROP TABLE works and leaves no MyRocks dictionary entries
DROP TABLE t1;
SELECT COUNT(*) AS leftover_ddl_entries FROM INFORMATION_SCHEMA.ROCKSDB_DDL
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1';
--echo # The table can be re-created with the same partition names
CREATE TABLE t1 (id INT PRIMARY KEY, v INT) ENGINE=ROCKSDB
  PARTITION BY RANGE (id) (
    PARTITION p0 VALUES LESS THAN (100),
    PARTITION p1 VALUES LESS THAN (200),
    PARTITION p2 VALUES LESS THAN (300),
    PARTITION p3 VALUES LESS THAN (400));
INSERT INTO t1 VALUES (1, 1), (101, 101), (201, 201), (301, 301);
SELECT COUNT(*) FROM t1;
DROP TABLE t1;
