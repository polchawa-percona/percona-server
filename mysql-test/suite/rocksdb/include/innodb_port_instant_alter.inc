# Runs an ALTER TABLE that the InnoDB original runs with ALGORITHM=INSTANT.
#
# MyRocks supports INSTANT only for appending columns (with
# rocksdb_enable_instant_ddl_for_append_column=ON) and for a few metadata-only
# changes. The statement is first tried with ALGORITHM=INSTANT, and the result
# file records whether MyRocks accepted it. If MyRocks rejects it, the same
# ALTER runs again with the default algorithm, so that the rest of the scenario
# (the data checks) still runs against the same table definition as in InnoDB.
#
# Usage:
#   --let $alter = ALTER TABLE t1 ADD COLUMN c4 INT
#   --source suite/rocksdb/include/innodb_port_instant_alter.inc
#
# $alter must not contain an ALGORITHM clause.

--echo $alter, ALGORITHM=INSTANT;
--disable_query_log
--disable_result_log
--error 0,ER_ALTER_OPERATION_NOT_SUPPORTED,ER_ALTER_OPERATION_NOT_SUPPORTED_REASON
--eval $alter, ALGORITHM=INSTANT
--let $_ipia_errno = $mysql_errno
--enable_result_log
--enable_query_log
if ($_ipia_errno == 0)
{
  --echo # MyRocks: INSTANT accepted
}
if ($_ipia_errno != 0)
{
  --echo # MyRocks: INSTANT rejected (error $_ipia_errno); running it with the default algorithm
  --disable_query_log
  --eval $alter
  --enable_query_log
}
