#
# Helper of rocksdb.ddl_concurrent_admin_ops*: picks the DDL for table t$tn in
# round $r and sends it on connection c$tn. The chosen operation number is
# returned in $ddl_op.
#
--expr $ddl_s = $r + $tn
--let $ddl_six = 6
--expr $ddl_op = $ddl_s % $ddl_six
--let $ddl = ALTER TABLE t$tn ADD INDEX kc (c), ALGORITHM=INPLACE
if ($ddl_op == 1)
{
  --let $ddl = ALTER TABLE t$tn DROP INDEX kc
}
if ($ddl_op == 2)
{
  --let $ddl = ALTER TABLE t$tn FORCE, ALGORITHM=COPY
}
if ($ddl_op == 3)
{
  --let $ddl = TRUNCATE TABLE t$tn
}
if ($ddl_op == 4)
{
  --let $ddl = RENAME TABLE t$tn TO tx$tn, tx$tn TO t$tn
}
if ($ddl_op == 5)
{
  --let $ddl = DROP TABLE t$tn
}
--connection c$tn
--send_eval $ddl
