# Runs every manual compaction trigger on the column families used by the
# mc3 tests. compact_lzero_now may fail with ER_WRONG_VALUE_FOR_VAR when an
# automatic compaction takes the L0 files first (PS-11747).
--disable_query_log
SET GLOBAL rocksdb_force_flush_memtable_and_lzero_now = 1;
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
--error 0,ER_WRONG_VALUE_FOR_VAR
SET GLOBAL rocksdb_compact_lzero_now = 1;
SET GLOBAL rocksdb_compact_cf = 'mc3_cf';
SET GLOBAL rocksdb_compact_cf = 'rev:mc3_rcf';
SET GLOBAL rocksdb_compact_cf = 'default';
--enable_query_log
