# Helper of errinj_loop.inc: restarts the server with its default options,
# without output, and restores what a restart loses ($errinj_after_restart,
# $errinj_after_restart_inc).
--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--shutdown_server 300
--source include/wait_until_disconnected.inc
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--source include/wait_until_connected_again.inc
--disable_query_log
--disable_result_log
if ($errinj_after_restart) {
  eval $errinj_after_restart;
}
--source $errinj_after_restart_inc
--disable_query_log
--disable_result_log
