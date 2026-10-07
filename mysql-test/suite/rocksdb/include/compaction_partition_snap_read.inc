# Runs the one-value query $sr_q in the current connection and sets $sr_out
# to its value, or to "ERR <errno>" if it fails with ER_TABLE_DEF_CHANGED or
# ER_UNKNOWN_PARTITION. The caller disables the query log.
--disable_result_log
--error 0, ER_TABLE_DEF_CHANGED, ER_UNKNOWN_PARTITION
eval $sr_q INTO @sr_v;
--enable_result_log
let $sr_err = $mysql_errno;
if ($sr_err)
{
  let $sr_out = ERR $sr_err;
}
if (!$sr_err)
{
  let $sr_out = `SELECT @sr_v`;
}
