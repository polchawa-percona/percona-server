# Statements that reach a table while it still has a pending inplace ALTER
# TABLE record (Rdb_key_def::DDL_PENDING_ALTER): after a crash inside the
# ALTER TABLE, before (rolled back) or after (committed) the server's data
# dictionary commit, run RENAME TABLE, DROP TABLE, TRUNCATE TABLE or LOCK
# TABLES before any other statement opens the table; let the resolution at
# open fail; let the reopen of the table after the data dictionary commit
# fail. Each case checks that the table matches the data dictionary and is
# usable, that no index data is left behind, and that no record is left
# (a stale record would later be applied to a new table of the same name).
#
# Usage:
#   CREATE TABLE orphan_probe ... (see ddl_crash_no_orphan_index.inc)
#   --let $ddl_cols = (id INT PRIMARY KEY, a INT, b INT, KEY ka(a))
#   --let $ddl_alter = ADD INDEX kb(b)
#   --source suite/rocksdb/include/ddl_crash_unresolved.inc
#
# The table has columns id, a, b and indexes from ka(a), kb(b).

--disable_query_log
CALL mtr.add_suppression("ALTER TABLE was interrupted before the data dictionary commit");
CALL mtr.add_suppression("failed to finish an interrupted ALTER TABLE");
--enable_query_log

--let $vdb = test
--let $ddl_hash_part = PARTITION BY HASH(id) PARTITIONS 2

--echo #
--echo # RENAME TABLE before the first open: the record moves with the table.
--echo # A new table with the old name is not affected by the record, also
--echo # after a restart.
--echo #
CREATE DATABASE db2;
--let $k = 0
while ($k < 5)
{
  --let $crash_point = crash_commit_after_prepare
  --let $ddl_part =
  --let $rename_db = test
  if ($k == 1)
  {
    --let $crash_point = crash_after_index_create
  }
  if ($k == 2)
  {
    --let $rename_db = db2
  }
  if ($k == 3)
  {
    --let $ddl_part = $ddl_hash_part
  }
  if ($k == 4)
  {
    --let $crash_point = crash_after_index_create
    --let $ddl_part = $ddl_hash_part
  }
  --let $st = t1_$k
  --source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
  --eval RENAME TABLE $st TO $rename_db.t2_$k
  --let $vdb = $rename_db
  --let $vt = t2_$k
  --source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
  --let $vdb = test
  --source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
  --eval CREATE TABLE $st $ddl_cols ENGINE=ROCKSDB $ddl_part
  --eval INSERT INTO $st VALUES (5,50,50)
  --eval SELECT * FROM $st ORDER BY id
  --inc $k
}
--source suite/rocksdb/include/ddl_crash_restart_no_pending.inc
--let $k = 0
while ($k < 5)
{
  --let $rename_db = test
  if ($k == 2)
  {
    --let $rename_db = db2
  }
  --eval SELECT * FROM t1_$k ORDER BY id
  --eval CHECK TABLE t1_$k, $rename_db.t2_$k
  --eval DROP TABLE t1_$k, $rename_db.t2_$k
  --inc $k
}
DROP DATABASE db2;

--echo #
--echo # DROP TABLE before the first open: the data of both index lists is
--echo # reclaimed and the record is removed: a new table with the same name
--echo # is not affected by it, also after a restart.
--echo #
--let $k = 0
while ($k < 3)
{
  --let $crash_point = crash_commit_after_prepare
  --let $ddl_part =
  if ($k == 1)
  {
    --let $crash_point = crash_after_index_create
  }
  if ($k == 2)
  {
    --let $ddl_part = $ddl_hash_part
  }
  --let $st = t1_$k
  --source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
  --eval DROP TABLE $st
  --source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
  --eval CREATE TABLE $st $ddl_cols ENGINE=ROCKSDB $ddl_part
  --eval INSERT INTO $st VALUES (5,50,50)
  --eval SELECT * FROM $st ORDER BY id
  --inc $k
}
--source suite/rocksdb/include/ddl_crash_restart_no_pending.inc
SELECT * FROM t1_0 ORDER BY id;
SELECT * FROM t1_1 ORDER BY id;
SELECT * FROM t1_2 ORDER BY id;
CHECK TABLE t1_0, t1_1, t1_2;
DROP TABLE t1_0, t1_1, t1_2;

--echo #
--echo # TRUNCATE TABLE before the first open: the new table has the index
--echo # list of the data dictionary, the data of the old one is reclaimed
--echo #
--let $k = 0
while ($k < 4)
{
  --let $crash_point = crash_commit_after_prepare
  --let $ddl_part =
  if ($k == 1)
  {
    --let $crash_point = crash_after_index_create
  }
  if ($k == 2)
  {
    --let $ddl_part = $ddl_hash_part
  }
  if ($k == 3)
  {
    --let $crash_point = crash_after_index_create
    --let $ddl_part = $ddl_hash_part
  }
  --let $st = t1_$k
  --source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
  --eval TRUNCATE TABLE $st
  --let $vt = $st
  --source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
  --source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
  --inc $k
}
--source suite/rocksdb/include/ddl_crash_restart_no_pending.inc
SELECT * FROM t1_0 ORDER BY id;
SELECT * FROM t1_3 ORDER BY id;
CHECK TABLE t1_0, t1_1, t1_2, t1_3;
DROP TABLE t1_0, t1_1, t1_2, t1_3;
--let $st = t1

--echo #
--echo # LOCK TABLES as the first open
--echo #
--let $vt = t1
--let $crash_point = crash_commit_after_prepare
--let $ddl_part =
--source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
LOCK TABLES t1 WRITE;
--source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
UNLOCK TABLES;
CHECK TABLE t1;
--source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
DROP TABLE t1;

--let $crash_point = crash_after_index_create
--source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
LOCK TABLES t1 READ;
SHOW CREATE TABLE t1;
SELECT * FROM t1 ORDER BY id;
--error 0,ER_KEY_DOES_NOT_EXITS
SELECT id, a FROM t1 FORCE INDEX(ka) WHERE a > 0 ORDER BY a;
--error 0,ER_KEY_DOES_NOT_EXITS
SELECT id, b FROM t1 FORCE INDEX(kb) WHERE b > 0 ORDER BY b;
--error ER_TABLE_NOT_LOCKED_FOR_WRITE
INSERT INTO t1 VALUES (4,40,40);
UNLOCK TABLES;
--source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
--source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
DROP TABLE t1;

--echo # LOCK TABLES fails while the resolution fails (debug point), then works
--let $crash_point = crash_commit_after_prepare
--source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
SET SESSION debug = "+d,rocksdb_fail_pending_alter_cleanup";
--error ER_GET_ERRMSG
LOCK TABLES t1 READ;
SET SESSION debug = "-d,rocksdb_fail_pending_alter_cleanup";
LOCK TABLES t1 WRITE;
--source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
UNLOCK TABLES;
--source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
DROP TABLE t1;

--echo # ALTER TABLE under LOCK TABLES whose cleanup fails (debug point): the
--echo # next ALTER TABLE under the same lock first finishes the cleanup, or
--echo # fails without changing anything
--eval CREATE TABLE t1 $ddl_cols ENGINE=ROCKSDB
INSERT INTO t1 VALUES (1,10,30),(2,20,20),(3,30,10);
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
LOCK TABLES t1 WRITE;
SET SESSION debug = "+d,rocksdb_fail_pending_alter_cleanup";
--eval ALTER TABLE t1 $ddl_alter, ALGORITHM=INPLACE
INSERT INTO t1 VALUES (4,40,40);
--error 0,ER_KEY_DOES_NOT_EXITS
SELECT id, b FROM t1 FORCE INDEX(kb) WHERE b > 0 ORDER BY b;
--error ER_INTERNAL_ERROR
ALTER TABLE t1 ADD INDEX kab(a, b), ALGORITHM=INPLACE;
SET SESSION debug = "-d,rocksdb_fail_pending_alter_cleanup";
ALTER TABLE t1 ADD INDEX kab(a, b), ALGORITHM=INPLACE;
SELECT id, a, b FROM t1 FORCE INDEX(kab) ORDER BY a, b;
UNLOCK TABLES;
CHECK TABLE t1;
--source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
--source suite/rocksdb/include/ddl_crash_restart_no_pending.inc
SHOW CREATE TABLE t1;
CHECK TABLE t1;
DROP TABLE t1;

--echo #
--echo # The resolution at open fails (debug point): the open fails, the record
--echo # stays (also over a restart) and the next open resolves it
--echo #
--source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
SET SESSION debug = "+d,rocksdb_fail_pending_alter_cleanup";
--error ER_GET_ERRMSG
SELECT * FROM t1;
SET SESSION debug = "-d,rocksdb_fail_pending_alter_cleanup";
--source include/restart_mysqld.inc
SELECT PARTITION_NAME, COUNT(*) AS rocksdb_indexes
  FROM information_schema.ROCKSDB_DDL
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'
  GROUP BY PARTITION_NAME ORDER BY PARTITION_NAME;
--source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
--source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
--source suite/rocksdb/include/ddl_crash_restart_no_pending.inc
DROP TABLE t1;

--echo # Partitioned: the open of p1 fails (debug point) after p0 was resolved
--let $ddl_part = $ddl_hash_part
--source suite/rocksdb/include/ddl_crash_unresolved_setup.inc
SET SESSION debug = "+d,rocksdb_open_fail_once_p1";
--error ER_GET_ERRNO
SELECT * FROM t1;
SET SESSION debug = "-d,rocksdb_open_fail_once_p1";
--echo # p0 is resolved, p1 is not
SELECT PARTITION_NAME, COUNT(*) AS rocksdb_indexes
  FROM information_schema.ROCKSDB_DDL
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'
  GROUP BY PARTITION_NAME ORDER BY PARTITION_NAME;
--source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
--source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
--source suite/rocksdb/include/ddl_crash_restart_no_pending.inc
DROP TABLE t1;

--echo #
--echo # The reopen of the table after the data dictionary commit fails (debug
--echo # point): the ALTER TABLE fails but is committed; the record is resolved
--echo # by the first open after a restart
--echo #
--eval CREATE TABLE t1 $ddl_cols ENGINE=ROCKSDB
--eval CREATE TABLE t2 $ddl_cols ENGINE=ROCKSDB $ddl_hash_part
INSERT INTO t1 VALUES (1,10,30),(2,20,20),(3,30,10);
INSERT INTO t2 VALUES (1,10,30),(2,20,20),(3,30,10);
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET SESSION debug = "+d,rocksdb_fail_reopen_after_alter";
--error ER_GET_ERRNO
--eval ALTER TABLE t1 $ddl_alter, ALGORITHM=INPLACE
SET SESSION debug = "-d,rocksdb_fail_reopen_after_alter";
--echo # Partitioned: only the reopen of p1 fails
SET SESSION debug = "+d,rocksdb_fail_reopen_after_alter_p1";
--error ER_GET_ERRNO
--eval ALTER TABLE t2 $ddl_alter, ALGORITHM=INPLACE
SET SESSION debug = "-d,rocksdb_fail_reopen_after_alter_p1";
--source include/restart_mysqld.inc
--let $assert_text = The records of t1 and of partition p1 of t2 are still pending after the restart
--let $assert_file = $MYSQLTEST_VARDIR/log/mysqld.1.err
--let $assert_select = Table 'test[.](t1|t2#P#p1)' has an unfinished ALTER TABLE
--let $assert_only_after = starting as process
--let $assert_count = 2
--source include/assert_grep.inc
--let $assert_text = No other record is pending
--let $assert_select = has an unfinished ALTER TABLE
--source include/assert_grep.inc
--let $vt = t1
--source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
--let $vt = t2
--source suite/rocksdb/include/ddl_crash_unresolved_verify.inc
--source suite/rocksdb/include/ddl_crash_no_orphan_index.inc
--source suite/rocksdb/include/ddl_crash_restart_no_pending.inc
DROP TABLE t1, t2;
--let $ddl_part =
