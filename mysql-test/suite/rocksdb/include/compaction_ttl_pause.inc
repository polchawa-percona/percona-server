# Start the manual compaction $ctp_trigger (a statement) in connection
# con_compact and hold it in the compaction filter factory with the debug
# point rocksdb_pause_compaction, then return to the default connection.
# Rows of t_fill are first rewritten and flushed, so every column family used
# by the test has an L0 file that overlaps older data, and the trigger starts
# a real compaction job rather than a trivial move.
--inc $ctp_n
eval REPLACE INTO t_fill VALUES (0, $ctp_n), (1, $ctp_n);
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
--let $ctl_pattern = MyRocks: test: compaction paused
--source compaction_ttl_log_count.inc
--let $ctp_before = $ctl_count
SET GLOBAL debug = '+d,rocksdb_pause_compaction';
connection con_compact;
--send_eval $ctp_trigger
connection default;
--let $ctp_tries = 600
while ($ctp_tries)
{
  --source compaction_ttl_log_count.inc
  if ($ctl_count > $ctp_before)
  {
    --let $ctp_tries = 0
  }
  if ($ctl_count <= $ctp_before)
  {
    --sleep 0.1
    --dec $ctp_tries
    if (!$ctp_tries)
    {
      --die Manual compaction was not paused
    }
  }
}
