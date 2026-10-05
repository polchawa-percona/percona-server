# Prints the outcome of the previous statement by its error code.
--let $rr_errno = $mysql_errno
--let $rr_outcome = OK
if ($rr_errno == 1213)
{
  --let $rr_outcome = ER_LOCK_DEADLOCK
}
if ($rr_errno == 1062)
{
  --let $rr_outcome = ER_DUP_ENTRY
}
if ($rr_errno == 1105)
{
  --let $rr_outcome = refused by the gap-lock rule
}
if ($rr_errno == 1205)
{
  --let $rr_outcome = ER_LOCK_WAIT_TIMEOUT
}
--echo # Outcome: $rr_outcome
