# Simulates a power cut and starts the server again.
#
# The server must run with
#   --rocksdb_fault_injection_options={"power_loss":true}
# so that writes to RocksDB files stay in memory until they are synced.
# Setting rocksdb_debug_simulate_power_loss then removes the files whose
# directory entries were never synced and kills the server, which loses every
# unsynced write, as a power cut does. InnoDB and binary log files are not
# covered: they keep what was written to the OS, as after a plain kill.
#
# Usage:
#   --let $restart_parameters = restart: ...   (optional)
#   --source suite/rocksdb/include/simulate_power_loss.inc

--source include/expect_crash.inc
--error CR_SERVER_LOST
SET GLOBAL rocksdb_debug_simulate_power_loss = ON;
--source include/wait_until_disconnected.inc
--source include/start_mysqld.inc
