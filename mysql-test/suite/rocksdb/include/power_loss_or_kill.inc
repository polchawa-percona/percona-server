# Crashes the server and starts it again: a simulated power cut (see
# simulate_power_loss.inc) when the server runs with
# rocksdb_fault_injection_options={"power_loss":true}, a plain kill
# otherwise. Both print the same output, so one result file covers both.

--disable_query_log
--let $_plk_power = `SELECT @@global.rocksdb_fault_injection_options LIKE '%power_loss%'`
--source include/expect_crash.inc
if ($_plk_power) {
  --disable_result_log
  --error CR_SERVER_LOST
  SET GLOBAL rocksdb_debug_simulate_power_loss = ON;
  --enable_result_log
}
if (!$_plk_power) {
  --shutdown_server 0
}
--source include/wait_until_disconnected.inc
--source include/start_mysqld.inc
--enable_query_log
