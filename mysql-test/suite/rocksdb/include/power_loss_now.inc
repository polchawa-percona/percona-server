# Crashes the server now and starts it again.
#
# The crash is fired through rocksdb_debug_crash_on_nth_durable_write at
# the checkpoint site: rocksdb::Checkpoint::Create() does no I/O, so the
# server aborts before anything is written or synced by this include.
# When the server runs with rocksdb_debug_unsynced_data_loss=ON, every
# write to RocksDB files that was not synced is lost, as in a power cut;
# otherwise this is a plain abort that keeps what reached the OS.
#
# Usage:
#   --let $restart_parameters = restart: ...   (optional)
#   --source suite/rocksdb/include/power_loss_now.inc

--disable_query_log
--source include/expect_crash.inc
SET SESSION rocksdb_debug_crash_on_nth_durable_write = 1;
--error CR_SERVER_LOST
eval SET GLOBAL rocksdb_create_checkpoint = '$MYSQLTEST_VARDIR/tmp/power_loss_never';
--source include/wait_until_disconnected.inc
--source include/start_mysqld.inc
--enable_query_log
