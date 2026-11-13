# Check that no index data is left behind by the DDL statements of a test:
# wait for the drop-index thread, flush and compact, then report every index
# number above $orphan_base that has SST data but belongs to no table.
#
# Usage:
#   (at the start of the test)
#   CREATE TABLE orphan_probe (a INT PRIMARY KEY) ENGINE=ROCKSDB;
#   --let $orphan_base = `SELECT INDEX_NUMBER - 1 FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 'orphan_probe'`
#   DROP TABLE orphan_probe;
#   ...
#   --source suite/rocksdb/include/ddl_crash_no_orphan_index.inc

--let $wait_timeout = 120
--let $wait_condition = SELECT COUNT(*) = 0 FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'DDL_DROP_INDEX_ONGOING'
--source include/wait_condition.inc
--disable_query_log
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_compact_cf = 'default';
--enable_query_log
--let $wait_timeout =
--echo # Index data that belongs to no table (expect 0):
--disable_query_log
--eval SELECT COUNT(DISTINCT f.COLUMN_FAMILY, f.INDEX_NUMBER) AS orphan_indexes FROM information_schema.ROCKSDB_INDEX_FILE_MAP f WHERE f.INDEX_NUMBER > $orphan_base AND NOT EXISTS (SELECT 1 FROM information_schema.ROCKSDB_DDL d WHERE d.INDEX_NUMBER = f.INDEX_NUMBER AND d.COLUMN_FAMILY = f.COLUMN_FAMILY)
--enable_query_log
