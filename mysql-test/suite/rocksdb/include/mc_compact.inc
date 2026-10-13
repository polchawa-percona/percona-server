# Runs one kind of manual compaction over all column families used by the mc
# tables. $mc_trigger is one of:
#   optimize    OPTIMIZE TABLE of the side tables in the same column
#               families (see mc_setup.inc)
#   compact_cf  SET GLOBAL rocksdb_compact_cf for each column family
#   lzero       memtable flush, then SET GLOBAL rocksdb_compact_lzero_now
#   flush_lzero SET GLOBAL rocksdb_force_flush_memtable_and_lzero_now
#   none        nothing (baseline for comparisons)
# The L0 triggers can fail with ER_WRONG_VALUE_FOR_VAR when an automatic
# compaction takes the L0 files first (PS-11747); that is accepted here.
--disable_query_log
--disable_result_log
if ($mc_trigger == optimize) {
  OPTIMIZE TABLE mc.side_def, mc.side_cf;
}
if ($mc_trigger == compact_cf) {
  SET GLOBAL rocksdb_compact_cf = 'default';
  SET GLOBAL rocksdb_compact_cf = 'mc_pk';
  SET GLOBAL rocksdb_compact_cf = 'mc_sk';
  SET GLOBAL rocksdb_compact_cf = 'rev:mc_rev';
}
if ($mc_trigger == lzero) {
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  --error 0, ER_WRONG_VALUE_FOR_VAR
  SET GLOBAL rocksdb_compact_lzero_now = 1;
}
if ($mc_trigger == flush_lzero) {
  --error 0, ER_WRONG_VALUE_FOR_VAR
  SET GLOBAL rocksdb_force_flush_memtable_and_lzero_now = 1;
}
--enable_result_log
--enable_query_log
