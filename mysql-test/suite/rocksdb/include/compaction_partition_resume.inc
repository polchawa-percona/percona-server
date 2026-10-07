# Releases the manual compaction started by compaction_partition_pause.inc
# and waits for it. rocksdb_compact_lzero_now and
# rocksdb_force_flush_memtable_and_lzero_now may fail with 1231 when an
# automatic compaction takes the L0 files first (PS-11747); set
# $cp_lzero_trigger = 1 for those, so that error is accepted and not printed.
SET GLOBAL debug = '-d,rocksdb_pause_compaction';
--connection con_mc
--echo # con_mc: reap
if ($cp_lzero_trigger)
{
  --disable_result_log
  --error 0, ER_WRONG_VALUE_FOR_VAR
  --reap
  --enable_result_log
}
if (!$cp_lzero_trigger)
{
  --reap
}
--connection default
--let $cp_lzero_trigger = 0
