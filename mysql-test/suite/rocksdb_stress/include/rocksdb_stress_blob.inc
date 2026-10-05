# Port of suite/innodb_stress/include/innodb_stress.inc for MyRocks.
#
# Populate table t1 with $num_records records and let the replica sync. Run
# $num_workers concurrent threads with OLTP transactions (BLOBs up to 24 KB
# when $use_blob is set) on the source. With $do_crash, kill the source after
# $kill_db_after seconds; with $kill_replica as well, kill the replica while
# it applies the backlog. After every round compare the checksums of t1 on
# the source and the replica, and repeat for $num_crashes rounds.

--let $PYTHON = python3

--mkdir $MYSQL_TMP_DIR/load_generator

--connection master

CREATE TABLE errors (msg TEXT) ENGINE=ROCKSDB;

--sync_slave_with_master

--connection master
--let $pid_file = `SELECT @@pid_file`
--let $crash_num = 0
--let $source_host = 127.0.0.1
--let $table = test
--let $user = root
--let $checksum = 0
--let $secondary_index_checks = 0

while ($num_crashes)
{
  connection master;
  if ($do_crash)
  {
    exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect;
  }
  if ($crash_num)
  {
    let $num_records = 0; # do not populate the table except for the first run.
  }

  let $exec =
$PYTHON $MYSQL_TEST_DIR/suite/rocksdb_stress/t/load_generator_blob.py $pid_file $kill_db_after
$num_records  $num_workers $num_transactions $user $source_host $MASTER_MYPORT
$table $use_blob $max_rows $MYSQL_TMP_DIR/load_generator 0 $checksum $secondary_index_checks;

  exec $exec >> $MYSQL_TMP_DIR/load_generator/errors.log;

  if ($do_crash)
  {
    --echo Wait for reconnect
    # Call script that will poll the server waiting for it to be back online again
    source include/wait_until_connected_again.inc;
    connection slave;
    source include/wait_until_connected_again.inc;
    connection master;
  }

  if ($kill_replica)
  {
    # Kill the replica while it is still applying the source's backlog, then
    # restart it and let it catch up.
    --echo Kill replica
    --let $rpl_server_number = 2
    --let $rpl_force_stop = 1
    --let $rpl_omit_print_server_parameters = 1
    --disable_query_log
    --disable_result_log
    --source include/rpl/restart_server.inc
    --enable_result_log
    --enable_query_log
    --let $rpl_force_stop = 0
    connection slave;
    --disable_warnings
    --disable_query_log
    --disable_result_log
    --source include/rpl/start_replica.inc
    --enable_result_log
    --enable_query_log
    --enable_warnings
    connection master;
  }

  --echo Checksum master
  let $master_checksum = query_get_value(CHECKSUM TABLE t1, Checksum, 1);

  # if sync_slave_with_master had a configurable timeout this would not be needed
  let $slave_sync_timeout = 7200;
  --source include/rpl/deprecated/wait_for_replica_to_sync_with_source.inc

  connection slave;
  --echo Checksum slave
  let $slave_checksum=query_get_value(CHECKSUM TABLE t1, Checksum, 1);
  let $not_same = `SELECT $master_checksum-$slave_checksum`;
  if ($not_same)
  {
    let $msg =
The checksums of table t1 for master and slave do not match for $crash_num th
crash. This may happen if there is a corrupt recovery log or a bug in crash
recovery. You can take a look at the logs in $MYSQL_TMP_DIR/load_generator to see the
queries issued before the crash.;
    echo $msg;

    connection master;
    eval select * into outfile '$MYSQLTEST_VARDIR/tmp/master_all' from t1 order by id;
    eval select id into outfile '$MYSQLTEST_VARDIR/tmp/master_id' from t1 order by id;
    show binary log status;

    connection slave;
    eval select * into outfile '$MYSQLTEST_VARDIR/tmp/slave_all' from t1 order by id;
    eval select id into outfile '$MYSQLTEST_VARDIR/tmp/slave_id' from t1 order by id;
    SHOW REPLICA STATUS;

    die;
  }
  dec $num_crashes;
  inc $crash_num;
}

# final cleanup
--connection master

let $primary=`select count(*) from t1 use index (primary)`;
let $secondary=`select count(*) from t1 use index (msg_i)`;
if ($primary != $secondary)
{
  --echo Secondary index inconsistent!  $primary != $secondary
  --die
}

--connection slave
let $primary=`select count(*) from t1 use index (primary)`;
let $secondary=`select count(*) from t1 use index (msg_i)`;
if ($primary != $secondary)
{
  --echo Secondary index inconsistent on the replica!  $primary != $secondary
  --die
}

--connection master
if (!$do_crash)
{
  --cat_file $MYSQL_TMP_DIR/load_generator/errors.log
  SELECT * FROM errors;
}

DROP TABLE t1;
DROP TABLE errors;

# if sync_slave_with_master had a configurable timeout this would not be needed
let $slave_sync_timeout = 7200;
--source include/rpl/deprecated/wait_for_replica_to_sync_with_source.inc

--connection slave
--source include/rpl/stop_replica.inc
# For stress tests sometimes the replication thread can not connect to master
# temporarily. This is either because the master crashed and it is recovering
# or the master is too busy and could not service the slave's requests.
# mtr's internal check requires that there be no errors in slave status.
# restarting replication clears the errors.
--source include/rpl/start_replica.inc
--source include/rpl/stop_replica.inc

connection master;

--force-rmdir $MYSQL_TMP_DIR/load_generator

# Reconnect client in case master had been killed
if ($kill_db_after)
{
  --let $rpl_server_number = 1
  --source include/rpl/reconnect.inc
}

--let $rpl_only_running_threads= 1
--source include/rpl/deinit.inc
