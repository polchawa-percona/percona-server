#
# Statement include for errinj_loop.inc: runs the admin statement
# $errinj_admin_stmt (OPTIMIZE TABLE and the like, on one table), which reports
# a failure in its result rows instead of as an error. A result whose first
# row is not "status" counts as an error.
#
if ($errinj_arm) {
  --source suite/rocksdb/include/errinj_arm.inc
}
--let $errinj_err = 0
--let $errinj_admin_type = query_get_value($errinj_admin_stmt, Msg_type, 1)
if ($errinj_admin_type != status) {
  --let $errinj_err = 1
}
if ($errinj_arm) {
  --let $errinj_left = `SELECT @@session.rocksdb_debug_inject_error_nth`
  SET SESSION rocksdb_debug_inject_error_nth = 0;
}
