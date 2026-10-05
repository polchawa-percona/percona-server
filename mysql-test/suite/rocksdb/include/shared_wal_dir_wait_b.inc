# Wait until instance B (socket $b_sock) answers, at most $wait_counter tenths of a second.
--disable_query_log
--let $_b_up= 0
while ($wait_counter)
{
  --error 0,1
  --exec $MYSQLADMIN --no-defaults -uroot -S $b_sock --connect-timeout=1 ping > /dev/null 2>&1
  if (!$__error)
  {
    --let $_b_up= 1
    --let $wait_counter= 0
  }
  if (!$_b_up)
  {
    --sleep 0.1
    --dec $wait_counter
  }
}
if (!$_b_up)
{
  --die Instance B did not start
}
--enable_query_log
