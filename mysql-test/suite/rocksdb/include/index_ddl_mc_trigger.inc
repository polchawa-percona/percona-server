# Runs one manual compaction of the kind selected by $mc_trigger:
#   1  OPTIMIZE TABLE $mc_optimize_table (compacts that table's index ranges)
#   2  SET GLOBAL rocksdb_compact_cf for the default and the reverse CF
#   3  SET GLOBAL rocksdb_compact_lzero_now
#   4  SET GLOBAL rocksdb_force_flush_memtable_and_lzero_now
# rocksdb_compact_lzero_now can fail with ER_WRONG_VALUE_FOR_VAR when automatic
# compaction takes the L0 files first (PS-11747); that is not checked here.
--disable_query_log
--disable_result_log
if ($mc_trigger == 1)
{
  eval OPTIMIZE TABLE $mc_optimize_table;
}
if ($mc_trigger == 2)
{
  SET GLOBAL rocksdb_compact_cf = 'default';
  SET GLOBAL rocksdb_compact_cf = 'rev:mc2r';
}
if ($mc_trigger == 3)
{
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  --error 0,ER_WRONG_VALUE_FOR_VAR
  SET GLOBAL rocksdb_compact_lzero_now = 1;
}
if ($mc_trigger == 4)
{
  SET GLOBAL rocksdb_force_flush_memtable_and_lzero_now = 1;
}
--enable_result_log
--enable_query_log
