# Create table $st (default t1) and crash the server inside an index-changing inplace ALTER
# TABLE at $crash_point, which leaves a pending ALTER TABLE record that the
# next open of the table resolves. Used by ddl_crash_unresolved.inc.
#
#   $ddl_cols, $ddl_alter  see ddl_crash_unresolved.inc
#   $crash_point           crash_commit_after_prepare (before the data
#                          dictionary commit: rolled back) or
#                          crash_after_index_create (after it: committed)
#   $ddl_part              optional partitioning clause
#   $st                    optional table name (default t1)

if (!$st)
{
  --let $st = t1
}
--eval CREATE TABLE $st $ddl_cols ENGINE=ROCKSDB $ddl_part
--eval INSERT INTO $st VALUES (1,10,30),(2,20,20),(3,30,10)
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
--let $crash_stmt = ALTER TABLE $st $ddl_alter, ALGORITHM=INPLACE
--source suite/rocksdb/include/ddl_crash.inc
--echo # Unresolved: MyRocks still has the index list of the ALTER TABLE
--eval SELECT PARTITION_NAME, COUNT(*) AS rocksdb_indexes FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$st' GROUP BY PARTITION_NAME ORDER BY PARTITION_NAME
