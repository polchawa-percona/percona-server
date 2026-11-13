#
# Checks that dropped indexes are reclaimed: the drop queue drains, and after
# a full compaction no SST file holds rows of an index that the MyRocks data
# dictionary does not know. Prints one line.
#
--disable_query_log
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
--source suite/rocksdb/include/drop_table_sync.inc
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_compact_cf = 'default';
--let $errinj_bg_orphans = `SELECT COUNT(DISTINCT m.COLUMN_FAMILY, m.INDEX_NUMBER) FROM information_schema.ROCKSDB_INDEX_FILE_MAP m LEFT JOIN information_schema.ROCKSDB_DDL d ON d.COLUMN_FAMILY = m.COLUMN_FAMILY AND d.INDEX_NUMBER = m.INDEX_NUMBER WHERE d.INDEX_NUMBER IS NULL AND m.NUM_ROWS > 0 AND m.COLUMN_FAMILY <> (SELECT NAME FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'CF_FLAGS' AND VALUE LIKE '\\_\\_system\\_\\_%')`
--enable_query_log
--echo # drop queue drained, indexes with orphaned rows: $errinj_bg_orphans
