#
# Runs errinj_loop.inc over the DML workloads for one class and status.
# Needs errinj_dml_setup.inc, $errinj_class and $errinj_status.
#

--let $errinj_stmt = INSERT INTO t1 VALUES (101, 101, 'ins', 1010), (102, 102, 'ins', 1020), (103, 103, 'ins', 1030)
--let $errinj_label = $errinj_class/$errinj_status multi-row INSERT
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = UPDATE t1 SET c = c + 1 WHERE pk <= 6
--let $errinj_label = $errinj_class/$errinj_status UPDATE of a secondary key column
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = UPDATE t1 SET pk = pk + 100 WHERE pk > 16
--let $errinj_label = $errinj_class/$errinj_status UPDATE of the primary key
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = DELETE FROM t1 WHERE pk BETWEEN 5 AND 9
--let $errinj_label = $errinj_class/$errinj_status DELETE
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO t1 VALUES (1, 51, 'dup', 1), (52, 52, 'new', 520) ON DUPLICATE KEY UPDATE c = c + 1000
--let $errinj_label = $errinj_class/$errinj_status INSERT ... ON DUPLICATE KEY UPDATE
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = REPLACE INTO t1 VALUES (2, 2, 'rep', 2), (60, 3, 'rep', 600)
--let $errinj_label = $errinj_class/$errinj_status REPLACE
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO t1 SELECT pk + 200, a + 200, b, c FROM t1 WHERE pk <= 8
--let $errinj_label = $errinj_class/$errinj_status INSERT ... SELECT
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO t_out SELECT * FROM t1 FORCE INDEX (kc) WHERE c >= 50
--let $errinj_label = $errinj_class/$errinj_status read through a secondary key
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO t_out SELECT STRAIGHT_JOIN t2.* FROM t1 FORCE INDEX (kb) JOIN t1 AS t2 FORCE INDEX (PRIMARY) ON t2.pk = t1.a WHERE t1.b = 'b3'
--let $errinj_label = $errinj_class/$errinj_status point reads in a join
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = CALL errinj_tx_rr()
--let $errinj_label = $errinj_class/$errinj_status transaction at REPEATABLE READ
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = CALL errinj_tx_rc()
--let $errinj_label = $errinj_class/$errinj_status transaction at READ COMMITTED
--source suite/rocksdb/include/errinj_loop.inc
