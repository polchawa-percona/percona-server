# Waits for the drop-index thread to reclaim every dropped partition, compacts
# every column family the tests use (bottommost level included), and reports
# index data in SST files that belongs to no index (expect none).
--echo # Reclaim of dropped partitions.
--disable_query_log
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
let $wait_timeout = 600;
let $wait_condition = SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
SET SESSION rocksdb_manual_compaction_bottommost_level = 'kForceOptimized';
SET GLOBAL rocksdb_compact_cf = 'default';
SET GLOBAL rocksdb_compact_cf = 'mc4_p0';
SET GLOBAL rocksdb_compact_cf = 'mc4_p0_sk';
SET GLOBAL rocksdb_compact_cf = 'rev:mc4_p1';
SET GLOBAL rocksdb_compact_cf = 'mc4_shared';
SET GLOBAL rocksdb_compact_cf = 'mc4_l0';
SET GLOBAL rocksdb_compact_cf = 'rev:mc4_l2';
SET SESSION rocksdb_manual_compaction_bottommost_level = DEFAULT;
let $cp_orphans = `SELECT COUNT(*) FROM INFORMATION_SCHEMA.ROCKSDB_INDEX_FILE_MAP m
  WHERE m.INDEX_NUMBER >= 256
    AND NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
                     WHERE d.COLUMN_FAMILY = m.COLUMN_FAMILY
                       AND d.INDEX_NUMBER = m.INDEX_NUMBER)`;
--echo index data in SST files that belongs to no index: $cp_orphans
if ($cp_orphans)
{
  SELECT m.COLUMN_FAMILY, m.INDEX_NUMBER, SUM(m.NUM_ROWS) AS rows_in_sst
    FROM INFORMATION_SCHEMA.ROCKSDB_INDEX_FILE_MAP m
   WHERE m.INDEX_NUMBER >= 256
     AND NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
                      WHERE d.COLUMN_FAMILY = m.COLUMN_FAMILY
                        AND d.INDEX_NUMBER = m.INDEX_NUMBER)
   GROUP BY m.COLUMN_FAMILY, m.INDEX_NUMBER;
}
--echo # MyRocks indexes per table (one per partition per index).
SELECT TABLE_NAME, COUNT(*) FROM INFORMATION_SCHEMA.ROCKSDB_DDL
 WHERE TABLE_SCHEMA = 'test' GROUP BY TABLE_NAME ORDER BY TABLE_NAME;
--enable_query_log
