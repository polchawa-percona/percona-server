# Runs one ALTER TABLE operation on InnoDB and on MyRocks with three
# settings of the instant DDL variables, and prints one line with the
# outcome of each run.
#
# Parameters:
#   $op    ALTER TABLE clause, without ALGORITHM/LOCK
#   $algo  algorithm/lock clause appended to $op (may be empty)
#   $tbl   name of the table after the ALTER (default: ins)
#   $skip_instant_off  if set, the "instant off" leg is not run and is
#          reported as SKIPPED (used where a known bug would be recorded)

--disable_query_log
--disable_result_log
--let $_aam_tbl = ins
if ($tbl)
{
  --let $_aam_tbl = $tbl
}

--let $_aam_leg = 1
while ($_aam_leg <= 4)
{
  --let $_aam_engine = ROCKSDB
  if ($_aam_leg == 1)
  {
    --let $_aam_engine = InnoDB
  }
  if ($_aam_leg == 2)
  {
    SET GLOBAL rocksdb_enable_instant_ddl = ON;
    SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = OFF;
    SET GLOBAL rocksdb_enable_instant_ddl_for_column_default_changes = OFF;
    SET GLOBAL rocksdb_enable_instant_ddl_for_table_comment_changes = OFF;
    SET GLOBAL rocksdb_enable_instant_ddl_for_drop_index_changes = OFF;
  }
  if ($_aam_leg == 3)
  {
    SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = ON;
    SET GLOBAL rocksdb_enable_instant_ddl_for_column_default_changes = ON;
    SET GLOBAL rocksdb_enable_instant_ddl_for_table_comment_changes = ON;
    SET GLOBAL rocksdb_enable_instant_ddl_for_drop_index_changes = ON;
  }
  if ($_aam_leg == 4)
  {
    SET GLOBAL rocksdb_enable_instant_ddl = OFF;
  }
  --let $_aam_run = 1
  if ($_aam_leg == 4)
  {
    if ($skip_instant_off)
    {
      --let $_aam_run = 0
      --let $_aam_r4 = SKIPPED
    }
  }
  if ($_aam_run)
  {

  eval CREATE TABLE ins (a INT PRIMARY KEY, b INT DEFAULT 1, c VARCHAR(10),
                         d INT, KEY kb(b)) ENGINE=$_aam_engine;
  INSERT INTO ins VALUES (1, 1, 'x', 4), (2, 2, 'y', 5);

  --disable_abort_on_error
  eval ALTER TABLE ins $op$algo;
  --enable_abort_on_error
  --let $_aam_res = OK
  if ($mysql_errno)
  {
    --let $_aam_res = $mysql_errname
  }
  if (!$mysql_errno)
  {
    # The rows must survive every accepted ALTER.
    --let $_aam_rows = `SELECT COUNT(*) FROM $_aam_tbl`
    if ($_aam_rows != 2)
    {
      --let $_aam_res = OK_BUT_ROWS=$_aam_rows
    }
  }
  DROP TABLE IF EXISTS ins;
  eval DROP TABLE IF EXISTS $_aam_tbl;

  if ($_aam_leg == 1)
  {
    --let $_aam_r1 = $_aam_res
  }
  if ($_aam_leg == 2)
  {
    --let $_aam_r2 = $_aam_res
  }
  if ($_aam_leg == 3)
  {
    --let $_aam_r3 = $_aam_res
  }
  if ($_aam_leg == 4)
  {
    --let $_aam_r4 = $_aam_res
  }
  }
  --inc $_aam_leg
}
SET GLOBAL rocksdb_enable_instant_ddl = ON;
SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = OFF;
SET GLOBAL rocksdb_enable_instant_ddl_for_column_default_changes = OFF;
SET GLOBAL rocksdb_enable_instant_ddl_for_table_comment_changes = OFF;
SET GLOBAL rocksdb_enable_instant_ddl_for_drop_index_changes = OFF;
--enable_result_log
--enable_query_log
--echo $op$algo | InnoDB: $_aam_r1 | MyRocks: $_aam_r2 | MyRocks instant on: $_aam_r3 | MyRocks instant off: $_aam_r4
--let $tbl =
