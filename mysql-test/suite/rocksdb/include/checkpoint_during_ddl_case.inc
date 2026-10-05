#
# One case of rocksdb.checkpoint_during_ddl.
#
# Pauses $ddl at the debug sync point $sync_point, creates a MyRocks
# checkpoint while it is paused, kills the server, replaces the MyRocks data
# directory with the checkpoint and restarts. This is what restoring a backup
# taken during the DDL looks like: the server data dictionary (InnoDB) is
# recovered to its state before the DDL committed, and the MyRocks data comes
# from the checkpoint. The table must then be usable with its original
# definition and all rows, with no leftover index.
#
# Parameters:
#   $case_name   label printed in the result
#   $sync_point  server or MyRocks debug sync point to pause at
#   $ddl         the ALTER TABLE statement on t1
#

--echo
--echo # Case: $case_name (paused at $sync_point)

CREATE TABLE t1 (a INT PRIMARY KEY, b INT, c VARCHAR(32), KEY kb (b))
  ENGINE=ROCKSDB;
--disable_query_log
let $i = 1;
BEGIN;
while ($i <= 2000)
{
  eval INSERT INTO t1 VALUES ($i, $i % 97, REPEAT(CHAR(97 + $i % 26), 20));
  inc $i;
}
COMMIT;
--enable_query_log
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SELECT COUNT(*), SUM(a), SUM(b) FROM t1;
--let $sql_tables_before = `SELECT COUNT(DISTINCT TABLE_SCHEMA, TABLE_NAME) FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME LIKE '#sql%'`

connect (con_ddl, localhost, root,,);
eval SET DEBUG_SYNC = '$sync_point SIGNAL ddl_paused WAIT_FOR never';
--send_eval $ddl

connection default;
SET DEBUG_SYNC = 'now WAIT_FOR ddl_paused';
--replace_result $checkpoint_dir CHECKPOINT
eval SET GLOBAL rocksdb_create_checkpoint = '$checkpoint_dir';

--echo # Kill the server while the DDL is paused and restore the checkpoint.
--source include/kill_mysqld.inc
--force-rmdir $rocksdb_dir
--move_file $checkpoint_dir $rocksdb_dir
--source include/start_mysqld.inc

connection con_ddl;
--error 0,CR_SERVER_LOST,CR_SERVER_GONE_ERROR
--reap
disconnect con_ddl;
connection default;

--echo # The table has its original definition and all rows.
SHOW CREATE TABLE t1;
SELECT COUNT(*), SUM(a), SUM(b) FROM t1;
SELECT COUNT(*), SUM(a), SUM(b) FROM t1 FORCE INDEX (kb) WHERE b >= 0;
CHECK TABLE t1;

--echo # MyRocks knows exactly the indexes of the server definition.
SELECT INDEX_NAME, INDEX_TYPE FROM information_schema.ROCKSDB_DDL
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1' ORDER BY INDEX_NAME;
--echo # A COPY ALTER leaves its #sql table in the MyRocks dictionary, as after
--echo # a crash during COPY ALTER (reported at startup by the PS-11680 fix).
--let $sql_tables_after = `SELECT COUNT(DISTINCT TABLE_SCHEMA, TABLE_NAME) FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME LIKE '#sql%'`
--expr $sql_tables_new = $sql_tables_after - $sql_tables_before
--echo New leftover #sql tables: $sql_tables_new

--echo # The table accepts DML and the DDL can be repeated.
INSERT INTO t1 VALUES (5000, 1, 'x');
UPDATE t1 SET b = b + 1 WHERE a <= 10;
DELETE FROM t1 WHERE a = 5000;
eval $ddl;
SELECT COUNT(*), SUM(a), SUM(b) FROM t1;
CHECK TABLE t1;
DROP TABLE t1;
