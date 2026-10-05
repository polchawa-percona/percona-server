# Lists the MyRocks row locks held on tables in schema `test`. This is the
# MyRocks counterpart of the performance_schema.data_locks queries in InnoDB
# tests: MyRocks registers no data lock inspector, so data_locks is always
# empty for MyRocks tables.
#
# Each row is one locked key. lock_key is the hex mem-comparable key after the
# 4-byte index number, e.g. 80000001 for INT 1 or 81 for TINYINT 1. A
# secondary key is followed by the primary key. MyRocks has no gap or table
# row locks, so there is no lock_type column.
#
# Usage:
#   [--let $show_locks_table = <table name>]
#   --source suite/rocksdb/include/innodb_port_show_row_locks.inc
#
--let $_sl_where = d.TABLE_SCHEMA = 'test'
if ($show_locks_table)
{
  --let $_sl_where = $_sl_where AND d.TABLE_NAME = '$show_locks_table'
}
--echo # Row locks held (INFORMATION_SCHEMA.ROCKSDB_LOCKS):
--disable_query_log
eval SELECT d.TABLE_NAME AS table_name, d.INDEX_NAME AS index_name,
       l.MODE AS lock_mode, SUBSTRING(l.`KEY`, 9) AS lock_key
FROM information_schema.ROCKSDB_LOCKS l
JOIN information_schema.ROCKSDB_DDL d
  ON d.COLUMN_FAMILY = l.COLUMN_FAMILY_ID
 AND d.INDEX_NUMBER = CONV(SUBSTRING(l.`KEY`, 1, 8), 16, 10)
WHERE $_sl_where
ORDER BY 1, 2, 3, 4;
--enable_query_log
--let $show_locks_table =
