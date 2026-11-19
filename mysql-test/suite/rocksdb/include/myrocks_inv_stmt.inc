# ==== Purpose ====
#
# Invariant 7: an error is truthful. Run one statement; if it fails, the
# table must be unchanged; if it succeeds and $inv_expect is given, the table
# must have exactly that checksum. Prints nothing when the invariant holds.
#
# Use it only for statements that are atomic for the table (autocommit, or a
# statement inside a transaction where a failed statement is rolled back).
#
# ==== Usage ====
#
# --let $inv_table = t1
# --let $inv_stmt = INSERT INTO t1 VALUES (1), (1)
# [--let $inv_errors = 0,ER_DUP_ENTRY]   # accepted outcomes, default 0
# [--let $inv_expect = 5:123456]         # checksum after success
# --source suite/rocksdb/include/myrocks_inv_stmt.inc
# --echo $inv_errno                      # errno of the statement, 0 on success

--source suite/rocksdb/include/myrocks_inv_table_checksum.inc
--let $_invt_before = $inv_checksum
--let $_invt_errors = $inv_errors
if (!$_invt_errors) {
  --let $_invt_errors = 0
}
--error $_invt_errors
--eval $inv_stmt
--let $inv_errno = $mysql_errno
--source suite/rocksdb/include/myrocks_inv_table_checksum.inc
if ($inv_errno) {
  if ($inv_checksum != $_invt_before) {
    --echo INVARIANT VIOLATION 7: statement failed with error $inv_errno but changed $inv_table ($_invt_before to $inv_checksum)
    --inc $inv_violations
  }
}
if (!$inv_errno) {
  if ($inv_expect) {
    if ($inv_checksum != $inv_expect) {
      --echo INVARIANT VIOLATION 7: statement succeeded but $inv_table is $inv_checksum, expected $inv_expect
      --inc $inv_violations
    }
  }
}
