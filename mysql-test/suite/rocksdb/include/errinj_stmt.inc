# Default statement include of errinj_loop.inc: runs $errinj_stmt, armed when
# $errinj_arm is 1, and sets $errinj_err and $errinj_left.
if (!$errinj_arm) {
  eval $errinj_stmt;
}
if ($errinj_arm) {
  --source suite/rocksdb/include/errinj_arm.inc
  --disable_abort_on_error
  eval $errinj_stmt;
  --let $errinj_err = $mysql_errno
  --enable_abort_on_error
  if ($errinj_err != 2013) {
    if ($errinj_err != 2006) {
      ROLLBACK;
      --let $errinj_left = `SELECT @@session.rocksdb_debug_inject_error_nth`
      SET SESSION rocksdb_debug_inject_error_nth = 0;
    }
  }
}
