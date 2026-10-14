# ==== Purpose ====
#
# Invariant 6: after a crash and restart, only a suffix of the most recent
# commits is lost, and none when the settings are durable.
#
# The scenario commits rows 1, 2, 3, ... into a log table, one commit per
# row (any other columns are allowed), and keeps in $inv_acked the highest
# sequence number whose commit returned success to the client. mysqltest
# variables survive the server restart, so the value is still known after
# the crash.
#
# Checks: the surviving rows are exactly 1..k for some k (no gaps, nothing
# from the future), and k >= $inv_acked when $inv_durable is set.
#
# ==== Usage ====
#
# --let $inv_log_table = commit_log   # column `seq` holds the sequence number
# --let $inv_acked = 1234
# [--let $inv_durable = 1]
# --source suite/rocksdb/include/myrocks_inv_crash_suffix.inc
# --echo $inv_survived                # k, for the test to report if needed

--disable_query_log
--let $_invx_row = `SELECT CONCAT(COUNT(*), ':', IFNULL(MIN(seq), 0), ':', IFNULL(MAX(seq), 0), ':', COUNT(DISTINCT seq)) FROM $inv_log_table`
--let $inv_survived = `SELECT IFNULL(MAX(seq), 0) FROM $inv_log_table`
--let $_invx_ok = `SELECT CONCAT(COUNT(*), ':', IF(COUNT(*) = 0, 0, 1), ':', COUNT(*), ':', COUNT(*)) = '$_invx_row' FROM $inv_log_table`
--enable_query_log
if (!$_invx_ok) {
  --echo INVARIANT VIOLATION 6: $inv_log_table is not a prefix 1..k (rows:min:max:distinct = $_invx_row)
  --inc $inv_violations
}
if ($inv_survived > $inv_acked) {
  # A commit that was in flight at the crash may survive; more may not.
  --let $_invx_extra = `SELECT $inv_survived - $inv_acked`
  if ($_invx_extra > 1) {
    --echo INVARIANT VIOLATION 6: $inv_log_table has $inv_survived rows but only $inv_acked commits were acknowledged
    --inc $inv_violations
  }
}
if ($inv_durable) {
  if ($inv_survived < $inv_acked) {
    --echo INVARIANT VIOLATION 6: durable settings lost commits: $inv_survived of $inv_acked acknowledged commits survived
    --inc $inv_violations
  }
}
