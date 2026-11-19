# Report whether the current transaction wrote keys to the database before
# COMMIT, i.e. whether write_unprepared flushed its write batch early.
# Usage:
#   --source suite/rocksdb/include/write_unprepared_flushed.inc   (records the start)
#   ... statements ...
#   --let $wup_check= 1
#   --source suite/rocksdb/include/write_unprepared_flushed.inc   (prints yes/no)
--let $_wup_q= SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = 'rocksdb_number_keys_written'
if (!$wup_check)
{
  --let $wup_k0= `$_wup_q`
}
if ($wup_check)
{
  --let $_wup_k1= `$_wup_q`
  --let $_wup_flushed= `SELECT IF($_wup_k1 > $wup_k0, 'yes', 'no')`
  --echo # Keys written to the database before COMMIT: $_wup_flushed
  --let $wup_check= 0
}
