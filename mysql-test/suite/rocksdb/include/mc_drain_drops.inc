# Waits until the drop-index thread has no ongoing drops and compacts cf_mc,
# which removes the data of the dropped indexes. Source it after dropping
# tables of cf_mc and before arming the next pause hook, so that a manual
# compaction of the drop-index thread cannot satisfy the next wait, and the
# keys of dropped indexes (which the compaction filter drops, and counts, at
# the front of the next compaction) do not hide the new data from the
# rocksdb_pause_manual_compaction_middle hook and from the output check.
--disable_query_log
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
let $wait_condition = SELECT COUNT(*) = 0 FROM information_schema.rocksdb_global_info
  WHERE type = 'DDL_DROP_INDEX_ONGOING';
--let $wait_timeout = 300
--source include/wait_condition.inc
SET GLOBAL rocksdb_compact_cf = 'cf_mc';
--enable_query_log
