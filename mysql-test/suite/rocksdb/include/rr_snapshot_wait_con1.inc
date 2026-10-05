# Waits until the statement sent on con1 ($con1_id) waits for a row lock.
# Uses the lock-wait view of the engine in $engine.
if ($engine == ROCKSDB)
{
  let $wait_condition = SELECT COUNT(*) = 1 FROM information_schema.rocksdb_trx
                        WHERE thread_id = $con1_id AND waiting_key != "";
}
if ($engine == InnoDB)
{
  let $wait_condition = SELECT COUNT(*) = 1 FROM information_schema.innodb_trx
                        WHERE trx_mysql_thread_id = $con1_id
                          AND trx_state = 'LOCK WAIT';
}
--source include/wait_condition.inc
