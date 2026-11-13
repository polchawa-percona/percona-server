# Reader on con_r at the server's isolation level: reads tn and ta, lets the
# caller's statements in $mc_between run (DML on tn and a manual compaction),
# and reads again. At REPEATABLE READ both reads must agree; at READ COMMITTED
# the second read must show the current data. ta cannot change meanwhile (the
# ALTER holds a shared lock), so its two reads must agree at both levels.
connection con_r;
BEGIN;
--let $mcr_tn1 = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(a), 0)) FROM tn FORCE INDEX (PRIMARY)`
--let $mcr_ta1 = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(a), 0)) FROM ta FORCE INDEX (ka0)`
connection default;
--disable_query_log
eval $mc_between;
--enable_query_log
--source suite/rocksdb/include/index_ddl_mc_trigger.inc
--let $mcr_now = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(a), 0)) FROM tn`
connection con_r;
--let $mcr_tn2 = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(a), 0)) FROM tn FORCE INDEX (ka)`
--let $mcr_ta2 = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(a), 0)) FROM ta FORCE INDEX (PRIMARY)`
COMMIT;
--let $mcr_exp = $mcr_tn1
if ($mc_iso == READ-COMMITTED)
{
  --let $mcr_exp = $mcr_now
}
if ($mcr_tn2 != $mcr_exp)
{
  --echo # MISMATCH reader on tn: got $mcr_tn2, expected $mcr_exp (first read $mcr_tn1, current $mcr_now)
}
if ($mcr_ta2 != $mcr_ta1)
{
  --echo # MISMATCH reader on ta: first read $mcr_ta1, second read $mcr_ta2
}
if ($mcr_tn2 == $mcr_exp)
{
  if ($mcr_ta2 == $mcr_ta1)
  {
    --echo # reader sees the expected rows
  }
}
connection default;
