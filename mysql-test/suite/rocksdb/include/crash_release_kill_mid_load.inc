# Starts a long statement in a separate connection and kills the server
# (SIGKILL through kill_and_restart_mysqld.inc, no debug points) once MyRocks
# has written $kml_rows rows, so the kill lands in the middle of the
# statement. Release-capable: it only uses the rocksdb_rows_inserted status
# counter (or another status counter), which exists in all builds.
#
# Parameters:
#   $kml_setup  SQL run in the loading connection before the statement
#               (session variables); may be empty
#   $kml_stmt   the long statement
#   $kml_rows   rows written before the kill (default 20000)
#   $kml_counter status counter to watch (default rocksdb_rows_inserted)

if (!$kml_rows)
{
  --let $kml_rows = 20000
}
if (!$kml_counter)
{
  --let $kml_counter = rocksdb_rows_inserted
}
--let $_kml_start = `SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = '$kml_counter'`

--connect (con_kml, localhost, root,,)
SET SESSION cte_max_recursion_depth = 4000000;
if ($kml_setup)
{
  --eval $kml_setup
}
--send_eval $kml_stmt

--connection default
--let $wait_timeout = 600
--let $wait_condition = SELECT VARIABLE_VALUE - $_kml_start >= $kml_rows FROM performance_schema.global_status WHERE VARIABLE_NAME = '$kml_counter'
--source include/wait_condition.inc

--let $do_not_echo_parameters = 1
--source include/kill_and_restart_mysqld.inc
--let $do_not_echo_parameters =
--disconnect con_kml
--connection default
--let $kml_setup =
--let $kml_rows =
--let $kml_counter =
