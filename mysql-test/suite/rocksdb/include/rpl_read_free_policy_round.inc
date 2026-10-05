# Restart the replica with write policy $replica_policy and read-free mode
# $read_free_mode, replay a PK/SK update/delete workload from the source with
# a multi-threaded applier, and check source and replica stay identical.
# Used by rpl_read_free_mixed_policies.test.

--let $rpl_server_number= 2
--let $rpl_omit_print_server_parameters= 1
--let $rpl_server_parameters= --rocksdb_write_policy=$replica_policy --rocksdb_write_batch_flush_threshold=$replica_threshold --rocksdb_read_free_rpl=$read_free_mode --replica_parallel_workers=4 --skip-replica-start
--source include/rpl/restart_server.inc
--source include/rpl/connection_replica.inc
--let $assert_text= Replica runs with write policy $replica_policy
--let $assert_cond= @@global.rocksdb_write_policy = "$replica_policy"
--source include/assert.inc
--source include/rpl/start_replica.inc

--source include/rpl/connection_source.inc
--disable_query_log
# Each round re-inserts into its own key range, so rounds never collide.
--inc $rf_round
--let $rf_base= `SELECT $rf_round * 100000`
let $i= 0;
while ($i < 6) {
  # Update primary keys, secondary keys, unique keys and plain columns,
  # delete by every key, then re-insert, in one multi-statement transaction
  # (written before commit under write_unprepared with threshold 1).
  BEGIN;
  eval UPDATE tp  SET a = a + 10000000 WHERE a < 10000000 AND a % 7 = $i;
  eval UPDATE tps SET b = b + 1000 WHERE a % 7 = $i;
  eval UPDATE tpu SET b = b + 10000000, c = c + 1 WHERE b < 10000000 AND a % 7 = $i;
  eval UPDATE ts  SET c = c + 1 WHERE b % 7 = $i;
  eval DELETE FROM tp  WHERE a % 11 = $i;
  eval DELETE FROM tps WHERE b % 11 = $i;
  eval DELETE FROM tpu WHERE b % 11 = $i;
  eval DELETE FROM ts  WHERE b % 11 = $i;
  COMMIT;
  eval INSERT INTO tp  SELECT seq + $rf_base + $i * 1000, seq FROM seq_200;
  eval INSERT INTO tps SELECT seq + $rf_base + $i * 1000, seq + $rf_base + $i * 1000, seq FROM seq_200;
  eval INSERT INTO tpu SELECT seq + $rf_base + $i * 1000, seq + $rf_base + $i * 1000, seq FROM seq_200;
  eval INSERT INTO ts  SELECT seq + $rf_base + $i * 1000, seq FROM seq_200;
  # A rolled-back transaction must leave no trace on the replica.
  BEGIN;
  eval UPDATE tps SET c = -1 WHERE a % 5 = $i;
  eval DELETE FROM tpu WHERE a % 3 = 0;
  ROLLBACK;
  inc $i;
}
--enable_query_log
--source include/rpl/sync_to_replica.inc

--let $diff_tables= master:tp, slave:tp
--source include/diff_tables.inc
--let $diff_tables= master:tps, slave:tps
--source include/diff_tables.inc
--let $diff_tables= master:tpu, slave:tpu
--source include/diff_tables.inc
--let $diff_tables= master:ts, slave:ts
--source include/diff_tables.inc

# Secondary indexes on the replica must agree with the primary key.
--source include/rpl/connection_replica.inc
--let $assert_text= Replica tps: secondary index matches primary key
--let $assert_cond= [SELECT COUNT(*) FROM tps FORCE INDEX(b)] = [SELECT COUNT(*) FROM tps FORCE INDEX(PRIMARY)]
--source include/assert.inc
--let $assert_text= Replica tpu: unique index matches primary key
--let $assert_cond= [SELECT COUNT(*) FROM tpu FORCE INDEX(b)] = [SELECT COUNT(*) FROM tpu FORCE INDEX(PRIMARY)]
--source include/assert.inc
--let $assert_text= No applier error on the replica
--let $assert_cond= "[SELECT LAST_ERROR_NUMBER FROM performance_schema.replication_applier_status_by_worker WHERE LAST_ERROR_NUMBER <> 0 LIMIT 1]" = ""
--source include/assert.inc
CHECK TABLE tp, tps, tpu, ts;
--source include/rpl/stop_replica.inc
