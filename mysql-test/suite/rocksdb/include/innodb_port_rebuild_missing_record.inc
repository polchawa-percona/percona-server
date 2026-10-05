#
# Helper of rocksdb.innodb_port_alter_table_rebuild_missing_record.
# Deletes rows while a consistent snapshot keeps the old versions (the snapshot
# reads no table, so it holds no metadata lock on t1), pauses an
# ALTER at $sync_point, releases the snapshot and compacts away the deleted
# versions, then lets the ALTER finish and checks that no row was lost.
#
# Parameters:
#   $delete_where  WHERE clause of the DELETE
#   $alter_stmt    the ALTER to run in con1
#   $sync_point    the debug_sync point to pause the ALTER at
#   $check_index   optional secondary index to compare with the primary key
#
--connection con2
START TRANSACTION WITH CONSISTENT SNAPSHOT;

--connection default
eval DELETE FROM t1 WHERE $delete_where;
--let $_rmr_before= `SELECT COUNT(*) FROM t1`
SET GLOBAL rocksdb_force_flush_memtable_now = ON;

--connection con1
eval SET DEBUG_SYNC='$sync_point SIGNAL paused WAIT_FOR go';
--echo # Send $alter_stmt
--send_eval $alter_stmt

--connection default
SET DEBUG_SYNC='now WAIT_FOR paused';
--connection con2
COMMIT;
--connection default
SET GLOBAL rocksdb_force_flush_memtable_now = ON;
SET GLOBAL rocksdb_compact_cf = 'default';
SET DEBUG_SYNC='now SIGNAL go';

--connection con1
--echo # Reap ALTER TABLE
--reap

--connection default
SET DEBUG_SYNC='RESET';
--let $_rmr_after= `SELECT COUNT(*) FROM t1`
if ($_rmr_before != $_rmr_after) {
  --echo # Records, before rebuild=$_rmr_before != after rebuild=$_rmr_after
  SELECT * FROM t1;
  --die Record count mismatch after the rebuild
}
--echo # Row count unchanged by the rebuild: $_rmr_after
CHECK TABLE t1;
if ($check_index) {
  --let $pk_check_table= t1
  --let $pk_check_index= $check_index
  --source suite/rocksdb/include/innodb_port_check_sk_vs_pk.inc
}
