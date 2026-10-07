#
# Runs $errinj_bg_stmt with rocksdb_debug_inject_bg_error = $errinj_bg_spec,
# where the injected error makes the background thread abort the server by
# design, waits for the abort, restarts the server and disables injection.
#
--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
eval SET GLOBAL rocksdb_debug_inject_bg_error = '$errinj_bg_spec';
--echo $errinj_bg_stmt;
# The statement may fail or succeed before the abort, or lose its connection.
--disable_query_log
--disable_result_log
--disable_abort_on_error
eval $errinj_bg_stmt;
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
--enable_abort_on_error
--enable_result_log
--enable_query_log
--source include/wait_until_disconnected.inc
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--source include/wait_until_connected_again.inc
