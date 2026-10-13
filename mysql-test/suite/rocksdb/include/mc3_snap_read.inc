# Reads $mc3_t through index $mc3_idx in the current session and classifies
# the result for a snapshot that predates a rebuild of the table:
#   empty_or_def_changed  0 rows, or ER_TABLE_DEF_CHANGED (PS-11627 changes
#                         the first into the second)
#   rows=<n>              anything else
# The raw outcome is appended to $MYSQLTEST_VARDIR/tmp/mc3_outcomes.txt.
--disable_query_log
--disable_result_log
--error 0,ER_TABLE_DEF_CHANGED
eval SELECT COUNT(*) INTO @mc3_c FROM $mc3_t FORCE INDEX ($mc3_idx);
--let $mc3_err = $mysql_errno
--enable_result_log
--let $mc3_c = error
if (!$mc3_err) {
  --let $mc3_c = `SELECT @mc3_c`
}
--exec echo "$mc3_scenario $mc3_level $mc3_idx: $mc3_c" >> $MYSQLTEST_VARDIR/tmp/mc3_outcomes.txt
--let $mc3_class = empty_or_def_changed
if (!$mc3_err) {
  if ($mc3_c) {
    --let $mc3_class = rows=$mc3_c
  }
}
--echo # $mc3_level read of $mc3_t via $mc3_idx: $mc3_class
--enable_query_log
