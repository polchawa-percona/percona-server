# Wakes the drop-index thread, waits until no drop is pending, compacts both
# column families fully and lists SST data of index ids that no longer exist.
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
let $wait_condition = SELECT COUNT(*) = 0 FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'DDL_DROP_INDEX_ONGOING';
--let $wait_timeout = 180
--source include/wait_condition.inc
--disable_query_log
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET @mc_save_bottommost = @@session.rocksdb_manual_compaction_bottommost_level;
SET SESSION rocksdb_manual_compaction_bottommost_level = 'kForce';
SET GLOBAL rocksdb_compact_cf = 'default';
SET GLOBAL rocksdb_compact_cf = 'rev:mc2r';
SET SESSION rocksdb_manual_compaction_bottommost_level = @mc_save_bottommost;
--enable_query_log
--echo # data of dropped indexes left in SST files
SELECT m.COLUMN_FAMILY, m.INDEX_NUMBER, m.NUM_ROWS FROM information_schema.ROCKSDB_INDEX_FILE_MAP m
  WHERE m.COLUMN_FAMILY <> 1 AND (m.COLUMN_FAMILY, m.INDEX_NUMBER) NOT IN (SELECT COLUMN_FAMILY, INDEX_NUMBER FROM information_schema.ROCKSDB_DDL);
SELECT TYPE, NAME FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE LIKE 'DDL%';
