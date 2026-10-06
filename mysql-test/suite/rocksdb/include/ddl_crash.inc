# Crash the server at a debug point while a DDL statement runs, then restart.
#
# Usage:
#   --let $crash_point = <DBUG keyword that calls DBUG_SUICIDE()>
#   --let $crash_stmt = <DDL statement>
#   [--let $restart_parameters = restart: <options>]
#   --source suite/rocksdb/include/ddl_crash.inc
#
# $restart_parameters is reset after the restart.

--echo # Crash at $crash_point during: $crash_stmt
--source include/expect_crash.inc
--disable_query_log
--eval SET SESSION debug = "+d,$crash_point"
--enable_query_log
--error CR_SERVER_LOST
--eval $crash_stmt
--source include/wait_until_disconnected.inc
--source include/start_mysqld.inc
--let $restart_parameters =
