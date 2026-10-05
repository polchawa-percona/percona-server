#
# Helper of rocksdb.ddl_concurrent_admin_ops*: after the round, recreates or
# repopulates t$tn if operation $ddl_op emptied or dropped it, then checks it.
#
if ($ddl_op == 5)
{
  if ($tn == 1)
  {
    eval $create_t1;
  }
  if ($tn == 2)
  {
    eval $create_t2;
  }
  if ($tn == 3)
  {
    eval $create_t3;
  }
}
if ($ddl_op >= 3)
{
  if ($ddl_op != 4)
  {
    eval INSERT INTO t$tn $load;
  }
}
--let $chk_table = t$tn
--let $chk_index = kb
--let $chk_rows = $nrows
--let $chk_sum = $sum
--source suite/rocksdb/include/ddl_concurrent_admin_ops_check.inc
