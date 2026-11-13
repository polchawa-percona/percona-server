# Starts the manual compaction $cp_trigger in connection con_mc and returns
# once it is held by the shared debug hook
# rocksdb_pause_manual_compaction_start. Memtables are flushed first, so the
# compaction has input files.
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
--let $cp_log_pattern = MyRocks: test: paused at rocksdb_pause_manual_compaction_start
--source suite/rocksdb/include/compaction_partition_log_count.inc
SET GLOBAL debug = '+d,rocksdb_pause_manual_compaction_start';
--connection con_mc
--echo # con_mc: $cp_trigger (held while it runs)
--send_eval $cp_trigger
--connection default
--source suite/rocksdb/include/compaction_partition_wait_log.inc
