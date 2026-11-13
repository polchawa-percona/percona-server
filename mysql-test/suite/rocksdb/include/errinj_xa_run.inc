#
# Statement include for errinj_loop.inc: one XA transaction 'x' that inserts,
# updates and deletes rows of tx, run as $errinj_xa_mode says:
#   same_commit      XA PREPARE and XA COMMIT on one connection
#   same_rollback    XA PREPARE and XA ROLLBACK on one connection
#   one_phase        XA COMMIT ONE PHASE
#   other_commit_p   XA PREPARE, disconnect, XA COMMIT on another connection;
#                    the injection is armed on the first connection
#   other_commit_c   the same, armed on the connection that commits
#   other_rollback_c XA PREPARE, disconnect, XA ROLLBACK on another connection,
#                    armed on that connection
# After an error the client gives the transaction up: it skips the remaining
# statements and runs XA END and XA ROLLBACK, whose errors are ignored. A
# transaction left prepared after an error is rolled back on a third
# connection; one that is left prepared after a successful run, or that
# cannot be rolled back, is a violation.
#
--let $errinj_err = 0
--let $errinj_xa_dead = 0
--let $errinj_xa_stop = 0
--let $errinj_xa_prepared = 0
--let $errinj_left = 1
--let $errinj_xa_second = 0
--let $errinj_xa_arm2 = 0
if ($errinj_xa_mode == other_commit_p) {
  --let $errinj_xa_second = COMMIT
}
if ($errinj_xa_mode == other_commit_c) {
  --let $errinj_xa_second = COMMIT
  --let $errinj_xa_arm2 = $errinj_arm
}
if ($errinj_xa_mode == other_rollback_c) {
  --let $errinj_xa_second = ROLLBACK
  --let $errinj_xa_arm2 = $errinj_arm
}
--let $errinj_xa_arm1 = $errinj_arm
if ($errinj_xa_arm2) {
  --let $errinj_xa_arm1 = 0
}

connect (con_xa, localhost, root,,);
if ($errinj_xa_arm1) {
  --source suite/rocksdb/include/errinj_arm.inc
}
--let $errinj_xa_q = XA START 'x'
--source suite/rocksdb/include/errinj_xa_stmt.inc
--let $errinj_xa_q = INSERT INTO tx VALUES (11, 11), (12, 12)
--source suite/rocksdb/include/errinj_xa_stmt.inc
--let $errinj_xa_q = UPDATE tx SET a = a + 100 WHERE pk = 3
--source suite/rocksdb/include/errinj_xa_stmt.inc
--let $errinj_xa_q = DELETE FROM tx WHERE pk = 4
--source suite/rocksdb/include/errinj_xa_stmt.inc
--let $errinj_xa_q = XA END 'x'
--source suite/rocksdb/include/errinj_xa_stmt.inc
if ($errinj_xa_mode == one_phase) {
  --let $errinj_xa_q = XA COMMIT 'x' ONE PHASE
  --source suite/rocksdb/include/errinj_xa_stmt.inc
}
if ($errinj_xa_mode != one_phase) {
  --let $errinj_xa_q = XA PREPARE 'x'
  --source suite/rocksdb/include/errinj_xa_stmt.inc
  if (!$errinj_xa_dead) {
    if (!$errinj_xa_e) {
      --let $errinj_xa_prepared = 1
    }
  }
}
if ($errinj_xa_mode == same_commit) {
  --let $errinj_xa_q = XA COMMIT 'x'
  --source suite/rocksdb/include/errinj_xa_stmt.inc
}
if ($errinj_xa_mode == same_rollback) {
  --let $errinj_xa_q = XA ROLLBACK 'x'
  --source suite/rocksdb/include/errinj_xa_stmt.inc
}
if ($errinj_xa_stop) {
  if (!$errinj_xa_dead) {
    --disable_abort_on_error
    XA END 'x';
    XA ROLLBACK 'x';
    --enable_abort_on_error
  }
}
if (!$errinj_xa_dead) {
  if ($errinj_xa_arm1) {
    --let $errinj_left = `SELECT @@session.rocksdb_debug_inject_error_nth`
    SET SESSION rocksdb_debug_inject_error_nth = 0;
  }
}
disconnect con_xa;

if ($errinj_xa_second) {
  if (!$errinj_xa_stop) {
    connect (con_xa2, localhost, root,,);
    if ($errinj_xa_arm2) {
      --source suite/rocksdb/include/errinj_arm.inc
    }
    --let $errinj_xa_q = XA $errinj_xa_second 'x'
    --source suite/rocksdb/include/errinj_xa_stmt.inc
    if (!$errinj_xa_dead) {
      if ($errinj_xa_arm2) {
        --let $errinj_left = `SELECT @@session.rocksdb_debug_inject_error_nth`
        SET SESSION rocksdb_debug_inject_error_nth = 0;
      }
    }
    disconnect con_xa2;
  }
}

# A server abort may only recover the final state once XA PREPARE succeeded.
if ($errinj_xa_dead) {
  if ($errinj_xa_prepared) {
    if ($errinj_xa_mode != same_rollback) {
      if ($errinj_xa_mode != other_rollback_c) {
        --let $errinj_must_be_after = 1
      }
    }
  }
}

if (!$errinj_xa_dead) {
  connect (con_xa3, localhost, root,,);
  --let $errinj_xa_left = query_get_value(XA RECOVER, data, 1)
  if ($errinj_xa_left == x) {
    if (!$errinj_err) {
      --echo VIOLATION $errinj_label n=$errinj_n: XA transaction left prepared after a successful run
      --inc $errinj_violations
    }
    --disable_abort_on_error
    XA ROLLBACK 'x';
    --enable_abort_on_error
    --let $errinj_xa_left = query_get_value(XA RECOVER, data, 1)
    if ($errinj_xa_left == x) {
      --echo VIOLATION $errinj_label n=$errinj_n: XA ROLLBACK of the prepared transaction failed
      --inc $errinj_violations
    }
  }
  disconnect con_xa3;
}
connection default;
