# Lets the drop-index thread run, waits until no index drop is pending,
# compacts every used column family fully and checks that no SST file
# holds keys of the indexes recorded in mc3.dropped, and that no table
# with a temporary name is left in the MyRocks dictionary.
--disable_query_log
SET @mc3_debug = @@global.debug;
SET GLOBAL debug = '-d,rocksdb_drop_index_thread_pause';
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
let $wait_condition = SELECT COUNT(*) = 0 FROM information_schema.rocksdb_global_info
  WHERE type = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
SET @mc3_bottommost = @@session.rocksdb_manual_compaction_bottommost_level;
SET SESSION rocksdb_manual_compaction_bottommost_level = 'kForce';
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_compact_cf = 'mc3_cf';
SET GLOBAL rocksdb_compact_cf = 'rev:mc3_rcf';
SET GLOBAL rocksdb_compact_cf = 'default';
SET SESSION rocksdb_manual_compaction_bottommost_level = @mc3_bottommost;
--enable_query_log
--echo # Keys of dropped indexes left in SST files (expect 0):
SELECT COUNT(*) AS dropped_index_entries
  FROM information_schema.rocksdb_index_file_map m
  JOIN mc3.dropped d ON m.column_family = d.cf AND m.index_number = d.idx;
--echo # Tables with temporary names in the MyRocks dictionary (expect none):
SELECT table_schema, table_name FROM information_schema.rocksdb_ddl
  WHERE table_name LIKE '#%' OR table_name LIKE '%#%';
