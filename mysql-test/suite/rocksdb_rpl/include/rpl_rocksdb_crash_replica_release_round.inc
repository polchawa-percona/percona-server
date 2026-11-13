# One round of rpl_rocksdb_crash_replica_release: build a backlog on the
# replica, kill the replica with SIGKILL while its parallel workers apply it
# and the source keeps writing, restart it, let it catch up and check the
# replica against the source.
#
# Parameters:
#   $round         round number; used for transaction ids and client seeds
#   $workers       replica_parallel_workers the replica runs with in this round
#   $kill_at       number of this round's transactions the replica must have
#                  applied before it is killed
#   $next_workers  replica_parallel_workers to restart the replica with
#   $load_script   path of the load generator written by the test

--echo # Round $round: kill a replica with $workers workers
--let $_cr_clients = 4
--let $_cr_txns = 250
--let $_cr_lo = `SELECT $round * 10000000`
--let $_cr_hi = `SELECT ($round + 1) * 10000000`

# Stop the applier so that a backlog builds up in the relay log.
--source include/rpl/connection_replica.inc
--let $_cr_w = `SELECT @@global.replica_parallel_workers`
--let $assert_text = replica runs with the expected number of workers
--let $assert_cond = $_cr_w = $workers
--source include/assert.inc
--source include/rpl/stop_applier.inc

--source include/rpl/connection_source.inc
--remove_files_wildcard $MYSQLTEST_VARDIR/tmp crash_replica_done_*
--let $_cr_c = 1
while ($_cr_c <= $_cr_clients)
{
  --let $_cr_seed = `SELECT $round * 100 + $_cr_c`
  --exec_in_background bash $load_script "$MYSQL" $round $_cr_c $_cr_seed $_cr_txns $MYSQLTEST_VARDIR/tmp/crash_replica_done_$_cr_c
  --inc $_cr_c
}
--let $wait_timeout = 300
--let $wait_condition = SELECT COUNT(*) >= 400 FROM r_log WHERE id >= $_cr_lo AND id < $_cr_hi
--source include/wait_condition.inc

# Start applying the backlog while the source keeps writing, and kill the
# replica once it has applied $kill_at of this round's transactions.
--source include/rpl/connection_replica.inc
--source include/rpl/start_applier.inc
--let $wait_timeout = 300
--let $wait_condition = SELECT COUNT(*) >= $kill_at FROM r_log WHERE id >= $_cr_lo AND id < $_cr_hi
--source include/wait_condition.inc
--let $_cr_workers_busy = `SELECT COUNT(*) FROM performance_schema.replication_applier_status_by_worker`
--let $assert_text = performance_schema lists one row per worker
--let $assert_cond = $_cr_workers_busy = $workers
--source include/assert.inc
--source include/kill_mysqld.inc

--let $rpl_server_number = 2
--let $rpl_server_parameters = --replica-parallel-workers=$next_workers
--let $rpl_omit_print_server_parameters = 1
--source include/rpl/start_server.inc
--source include/rpl/connection_replica.inc
--source include/rpl/start_replica.inc

# Wait for the clients, then for the replica to catch up.
--perl
  my $dir = "$ENV{MYSQLTEST_VARDIR}/tmp";
  for (1 .. 6000) {
    my $done = grep { -e "$dir/crash_replica_done_$_" } 1 .. 4;
    exit 0 if $done == 4;
    select(undef, undef, undef, 0.1);
  }
  die "load generators did not finish";
EOF
--source include/rpl/connection_source.inc
--let $_cr_round_txns = `SELECT COUNT(*) FROM r_log WHERE id >= $_cr_lo AND id < $_cr_hi`
--let $assert_text = the round's load committed transactions on the source
--let $assert_cond = $_cr_round_txns >= 500
--source include/assert.inc
--source include/rpl/sync_to_replica.inc

# The replica equals the source.
--let $diff_tables = master:r_acc, slave:r_acc
--source include/diff_tables.inc
--let $diff_tables = master:r_log, slave:r_log
--source include/diff_tables.inc
--let $diff_tables = master:r_ev, slave:r_ev
--source include/diff_tables.inc
--let $diff_tables = master:i_acc, slave:i_acc
--source include/diff_tables.inc
--let $diff_tables = master:i_log, slave:i_log
--source include/diff_tables.inc

--source include/rpl/connection_source.inc
--let $_cr_src_gtids = `SELECT @@global.gtid_executed`
--source include/rpl/connection_replica.inc
# Leave out the replica's own GTIDs (from mtr.add_suppression on the replica).
--let $_cr_rep_gtids = `SELECT GTID_SUBTRACT(@@global.gtid_executed, CONCAT(@@global.server_uuid, ':1-1000000000'))`
--let $assert_text = the replica executed exactly the source's GTIDs
--let $assert_cond = GTID_SUBSET("$_cr_src_gtids", "$_cr_rep_gtids") AND GTID_SUBSET("$_cr_rep_gtids", "$_cr_src_gtids")
--source include/assert.inc

# No transaction was applied twice or lost on the replica.
SELECT SUM(bal) = 0 AS rocksdb_zero_sum,
       SUM(moves) = 2 * (SELECT COUNT(*) FROM r_log) AS rocksdb_moves_match_log
  FROM r_acc;
SELECT SUM(bal) = 0 AS innodb_zero_sum,
       SUM(moves) = 2 * (SELECT COUNT(*) FROM i_log) AS innodb_moves_match_log
  FROM i_acc;
SELECT COUNT(*) AS mixed_only_in_rocksdb FROM r_log
  WHERE id % 2 = 0 AND id NOT IN (SELECT id FROM i_log);
SELECT COUNT(*) AS mixed_only_in_innodb FROM i_log
  WHERE id NOT IN (SELECT id FROM r_log);

# MyRocks secondary indexes agree with the primary key on the replica.
SELECT (SELECT GROUP_CONCAT(id, ':', bal ORDER BY id) FROM r_acc FORCE INDEX (kbal)
          WHERE bal > -1000000) =
       (SELECT GROUP_CONCAT(id, ':', bal ORDER BY id) FROM r_acc FORCE INDEX (PRIMARY))
  AS acc_sk_matches_pk;
SELECT (SELECT COUNT(*) FROM r_ev FORCE INDEX (kk) WHERE k >= 0) =
       (SELECT COUNT(*) FROM r_ev FORCE INDEX (PRIMARY)) AS ev_sk_count_matches_pk,
       (SELECT SUM(k) FROM r_ev FORCE INDEX (kk) WHERE k >= 0) =
       (SELECT SUM(k) FROM r_ev FORCE INDEX (PRIMARY)) AS ev_sk_sum_matches_pk;
CHECK TABLE r_acc, r_log, r_ev;

--let $_cr_err = query_get_value(SELECT LAST_ERROR_NUMBER FROM performance_schema.replication_applier_status_by_coordinator, LAST_ERROR_NUMBER, 1)
--let $assert_text = the applier reports no error
--let $assert_cond = $_cr_err = 0
--source include/assert.inc

--source include/rpl/connection_source.inc
--remove_files_wildcard $MYSQLTEST_VARDIR/tmp crash_replica_done_*
