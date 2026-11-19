#
# Workloads of errinj_crash.test for one class and status.
#

--let $errinj_stmt = INSERT INTO t1 VALUES (101, 101, 'ins', 1010), (102, 102, 'ins', 1020)
--let $errinj_label = $errinj_class/$errinj_status multi-row INSERT
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = UPDATE t1 SET pk = pk + 100, c = c + 1 WHERE pk > 18
--let $errinj_label = $errinj_class/$errinj_status UPDATE of the primary key
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = CALL errinj_tx_rr()
--let $errinj_label = $errinj_class/$errinj_status transaction at REPEATABLE READ
--source suite/rocksdb/include/errinj_loop.inc
