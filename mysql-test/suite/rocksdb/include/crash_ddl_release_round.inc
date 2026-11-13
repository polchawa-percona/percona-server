# One kill round for crash_ddl_release (release-variant item R6).
#
# Runs "ALTER TABLE t1 $ddl_alter" on a second connection, kills the server with SIGKILL
# while the ALTER is running, restarts it and checks the invariants in
# crash_ddl_release_check.inc. If the table came back with its old definition, the ALTER is
# retried and must then succeed.
#
# Parameters:
#   $ddl_label       description printed in the result
#   $ddl_alter       the ALTER TABLE clause
#   $ddl_kill_state  PROCESSLIST state to kill in, e.g. "copy to tmp table";
#                    empty = kill as soon as the ALTER is running
#   $ddl_parts       number of partitions of t1 (1 if not partitioned)
#
# Updates $ddl_killed_old (rounds where the ALTER was interrupted and the old definition
# came back).

--echo
--echo # Round: $ddl_label
--disable_query_log

# Signature the ALTER should produce, from an empty copy of t1.
CREATE TABLE t_ref LIKE t1;
eval ALTER TABLE t_ref $ddl_alter;
--let $ddl_sig_table = t_ref
--source suite/rocksdb/include/crash_ddl_release_sig.inc
--let $ddl_sig_new = $ddl_sig
DROP TABLE t_ref;

--source suite/rocksdb/include/crash_ddl_release_sig.inc
--let $ddl_sig_old = $ddl_sig
--let $ddl_data = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b, pad)))) FROM t1 FORCE INDEX (PRIMARY)`

connect (con1, localhost, root,,test);
--let $ddl_con1_id = `SELECT CONNECTION_ID()`
send_eval ALTER TABLE t1 $ddl_alter;

connection default;
# Wait until the ALTER runs in the requested state, polling every 10 ms for at most 5 minutes.
# Before the ALTER starts and after it ends, INFO is empty; once the ALTER has been seen
# running, an empty INFO means it has finished.
--let $ddl_seen = timeout
--let $ddl_started = 0
--let $ddl_n = 0
--let $ddl_go = 1
while ($ddl_go)
{
  --let $ddl_st = `SELECT CONCAT(IF(INFO LIKE 'ALTER TABLE t1%', 'A', '-'), '/', IFNULL(STATE, '')) FROM information_schema.PROCESSLIST WHERE ID = $ddl_con1_id`
  if (`SELECT '$ddl_st' LIKE 'A/%'`)
  {
    --let $ddl_started = 1
    if (`SELECT '$ddl_st' = 'A/$ddl_kill_state'`)
    {
      --let $ddl_seen = kill_state
      --let $ddl_go = 0
    }
    if (!$ddl_kill_state)
    {
      --let $ddl_seen = running
      --let $ddl_go = 0
    }
  }
  if ($ddl_go)
  {
    if ($ddl_started)
    {
      if (`SELECT '$ddl_st' LIKE '-/%'`)
      {
        --let $ddl_seen = finished
        --let $ddl_go = 0
      }
    }
  }
  if ($ddl_go)
  {
    --sleep 0.01
    --inc $ddl_n
    if ($ddl_n == 30000)
    {
      --let $ddl_go = 0
    }
  }
}
if ($ddl_seen == timeout)
{
  --echo # ALTER never reached state "$ddl_kill_state" (last: $ddl_st)
  --die ALTER did not reach the kill state
}
--enable_query_log

--source include/kill_mysqld.inc
--source include/start_mysqld.inc

connection con1;
--disable_query_log
--disable_result_log
--error 0,CR_SERVER_LOST,CR_SERVER_GONE_ERROR
reap;
--let $ddl_errno = $mysql_errno
--enable_result_log
--enable_query_log
disconnect con1;
connection default;

--exec echo "$ddl_label | seen=$ddl_seen st=$ddl_st polls=$ddl_n errno=$ddl_errno" >> $MYSQLTEST_VARDIR/log/crash_ddl_release.trace
--source suite/rocksdb/include/crash_ddl_release_check.inc
--exec echo "  -> state=$ddl_state orphans_so_far=$ddl_orphans_total" >> $MYSQLTEST_VARDIR/log/crash_ddl_release.trace

# An ALTER that reported success before the kill must have survived it.
if ($ddl_errno == 0)
{
  if ($ddl_state != new)
  {
    --die An ALTER that completed before the crash was lost
  }
}

# If the ALTER was interrupted, retry it; it must succeed. Either way the table must end
# the round consistent and with the new definition. The output is the same in both cases.
if ($ddl_state == old)
{
  --inc $ddl_killed_old
  --disable_query_log
  eval ALTER TABLE t1 $ddl_alter;
  --enable_query_log
}
--source suite/rocksdb/include/crash_ddl_release_check.inc
--let $assert_text = After the round the table has the new definition
--let $assert_cond = "$ddl_state" = "new"
--source include/assert.inc
--echo # Round done: the table is consistent and has the new definition
