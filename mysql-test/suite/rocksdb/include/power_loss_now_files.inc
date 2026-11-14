# Like power_loss_now.inc, but also checks the temporary bulk load SST files
# while the server is down (before the restart removes them), see
# power_loss_tmp_files.pl. Evidence that dropping the unsynced directory
# entries removes a real file: they are there after a plain kill, gone after
# the power loss.
#
--disable_query_log
--let $_plf_power = `SELECT @@global.rocksdb_debug_unsynced_data_loss`
--let $_plf_dir = `SELECT CONCAT(@@datadir, '.rocksdb')`
--source include/expect_crash.inc
SET SESSION rocksdb_debug_crash_on_nth_durable_write = 1;
--error CR_SERVER_LOST
eval SET GLOBAL rocksdb_create_checkpoint = '$MYSQLTEST_VARDIR/tmp/power_loss_never';
--source include/wait_until_disconnected.inc
--enable_query_log
--exec perl $MYSQL_TEST_DIR/suite/rocksdb/include/power_loss_tmp_files.pl $_plf_dir $_plf_power
--disable_query_log
--source include/start_mysqld.inc
--enable_query_log
