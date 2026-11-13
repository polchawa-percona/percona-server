# Wait until the connection with id $wait_conn_id is waiting for a MyRocks row lock.
# MyRocks has no "lock_wait_will_wait" debug sync point (InnoDB tests use it); a
# waiting transaction shows a non-empty WAITING_KEY in INFORMATION_SCHEMA.ROCKSDB_TRX.
#
# Usage:
#   --let $wait_conn_id= <connection id>
#   --source suite/rocksdb/include/innodb_port_wait_for_lock.inc
--let $wait_condition= SELECT COUNT(*) = 1 FROM information_schema.rocksdb_trx WHERE thread_id = $wait_conn_id AND waiting_key != ''
--source include/wait_condition.inc
