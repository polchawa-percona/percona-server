# Wait until instance B (socket $b_sock) stops answering after a shutdown request,
# at most 30 s.
--disable_query_log
--let $_b_wait= 300
while ($_b_wait)
{
  --error 0,1
  --exec $MYSQLADMIN --no-defaults -uroot -S $b_sock --connect-timeout=1 ping > /dev/null 2>&1
  if ($__error)
  {
    --let $_b_wait= 1
  }
  if (!$__error)
  {
    --sleep 0.1
  }
  --dec $_b_wait
}
--enable_query_log
