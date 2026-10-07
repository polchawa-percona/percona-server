# Runs one DDL statement, crashes the server either after it completes or
# while it waits at a DEBUG_SYNC point, and prints the state after recovery.
#
# $pl_create  CREATE TABLE statement for t1 (optional, a default is used)
# $pl_ddl     the DDL statement
# $pl_sync    DEBUG_SYNC point to crash at (optional, empty = after the DDL)
# $pl_mode_override  rocksdb_flush_log_at_trx_commit for the DDL (optional)
#
# The crash is a simulated power cut when the server runs with
# rocksdb_fault_injection_options={"power_loss":true}, otherwise a plain kill.
# The rows are loaded with rocksdb_flush_log_at_trx_commit=1, the DDL runs
# with the server's configured value.

--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS t1, t2;
--enable_warnings
--let $_pl_mode = `SELECT @@global.rocksdb_flush_log_at_trx_commit`
SET GLOBAL rocksdb_flush_log_at_trx_commit = 1;
if (!$pl_create) {
  --let $_pl_create = CREATE TABLE t1 (id INT PRIMARY KEY, a INT, b VARCHAR(32), KEY ka (a)) ENGINE=ROCKSDB
}
if ($pl_create) {
  --let $_pl_create = $pl_create
}
eval $_pl_create;
INSERT INTO t1 SELECT seq, seq % 50, CONCAT('row', seq) FROM (WITH RECURSIVE s(seq) AS (SELECT 1 UNION ALL SELECT seq + 1 FROM s WHERE seq < 500) SELECT seq FROM s) AS x;
if ($pl_mode_override) {
  --let $_pl_mode = $pl_mode_override
}
eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $_pl_mode;
# CREATE TABLE ... SELECT would need a gap lock at REPEATABLE READ.
SET SESSION transaction_isolation = 'READ-COMMITTED';
--enable_query_log

if ($pl_sync) {
  --echo # $pl_ddl, crash at $pl_sync
  connect (con_pl, localhost, root,,);
  --disable_query_log
  SET SESSION transaction_isolation = 'READ-COMMITTED';
  eval SET DEBUG_SYNC = '$pl_sync SIGNAL pl_reached WAIT_FOR pl_never';
  --enable_query_log
  --send_eval $pl_ddl
  connection default;
  SET DEBUG_SYNC = 'now WAIT_FOR pl_reached';
}
if (!$pl_sync) {
  --echo # $pl_ddl, crash after it completes
  eval $pl_ddl;
}

--disable_query_log
--source ../include/power_loss_or_kill.inc
--enable_query_log
if ($pl_sync) {
  disconnect con_pl;
}

SELECT table_name FROM information_schema.tables
  WHERE table_schema = 'test' ORDER BY table_name;
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't1'`) {
  SHOW CREATE TABLE t1;
  SELECT COUNT(*), SUM(id), SUM(a) FROM t1;
  --replace_column 2 # 3 #
  CHECK TABLE t1;
}
if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = 't2'`) {
  SHOW CREATE TABLE t2;
  SELECT COUNT(*), SUM(id), SUM(a) FROM t2;
  --replace_column 2 # 3 #
  CHECK TABLE t2;
}
--let $pl_create =
--let $pl_sync =
--let $pl_mode_override =
