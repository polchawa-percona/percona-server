#
# Runs errinj_loop.inc over the XA workloads (see errinj_xa_run.inc) for one
# class and status. Needs errinj_xa_setup.inc, $errinj_class and
# $errinj_status.
#
--let $errinj_xa_recover = COMMIT
--let $errinj_xa_mode = same_commit
--let $errinj_label = $errinj_class/$errinj_status XA PREPARE and XA COMMIT
--source suite/rocksdb/include/errinj_loop.inc
--let $errinj_xa_mode = one_phase
--let $errinj_label = $errinj_class/$errinj_status XA COMMIT ONE PHASE
--source suite/rocksdb/include/errinj_loop.inc
--let $errinj_xa_mode = other_commit_p
--let $errinj_label = $errinj_class/$errinj_status XA PREPARE, XA COMMIT from another connection
--source suite/rocksdb/include/errinj_loop.inc
--let $errinj_xa_mode = other_commit_c
--let $errinj_label = $errinj_class/$errinj_status XA COMMIT of a prepared transaction from another connection
--source suite/rocksdb/include/errinj_loop.inc
--let $errinj_xa_recover = ROLLBACK
--let $errinj_xa_mode = same_rollback
--let $errinj_label = $errinj_class/$errinj_status XA PREPARE and XA ROLLBACK
--source suite/rocksdb/include/errinj_loop.inc
--let $errinj_xa_mode = other_rollback_c
--let $errinj_label = $errinj_class/$errinj_status XA ROLLBACK of a prepared transaction from another connection
--source suite/rocksdb/include/errinj_loop.inc
