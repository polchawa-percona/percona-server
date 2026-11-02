# Restarts the server with rocksdb_log_dir set to a directory MyRocks must
# refuse, and checks that MyRocks initialization fails with an error.
#
# Usage:
#   --let $log_dir_opts = --rocksdb_log_dir=...
#   --let $log_dir_error = <regex of the expected error log line>
#   --source suite/rocksdb/include/log_dir_rejected.inc
#
# The server is left running without MyRocks.

--source include/shutdown_mysqld.inc
--let $restart_parameters = restart: $log_dir_opts
--replace_result $MYSQL_TMP_DIR MYSQL_TMP_DIR $MYSQLD_DATADIR MYSQLD_DATADIR
--source include/start_mysqld.inc
SELECT COUNT(*) FROM information_schema.ENGINES WHERE ENGINE = 'ROCKSDB';
--let $assert_text = MyRocks reports why rocksdb_log_dir is refused
--let $assert_file = $MYSQLTEST_VARDIR/log/mysqld.1.err
--let $assert_select = $log_dir_error
--let $assert_count = 1
--let $assert_only_after = CURRENT_TEST: rocksdb.$log_dir_test
--source include/assert_grep.inc
