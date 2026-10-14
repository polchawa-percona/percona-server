#
# Runs errinj_loop.inc over the TTL workloads for one class and status.
# Needs errinj_ttl_setup.inc, $errinj_class and $errinj_status.
#

--let $errinj_stmt = INSERT INTO tt_h VALUES (2, 200, 'new'), (20, 20, 'new')
--let $errinj_label = $errinj_class/$errinj_status INSERT over an expired row (hidden TTL)
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO tt_c VALUES (3, 300, 'new', UNIX_TIMESTAMP()), (20, 20, 'new', UNIX_TIMESTAMP())
--let $errinj_label = $errinj_class/$errinj_status INSERT over an expired row (TTL column)
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = UPDATE tt_h SET b = 'upd', a = a + 1 WHERE pk BETWEEN 3 AND 9
--let $errinj_label = $errinj_class/$errinj_status UPDATE of expired and unexpired rows (hidden TTL)
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = UPDATE tt_c SET a = a + 1 WHERE pk BETWEEN 3 AND 9
--let $errinj_label = $errinj_class/$errinj_status UPDATE of expired and unexpired rows (TTL column)
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = DELETE FROM tt_h WHERE pk BETWEEN 4 AND 8
--let $errinj_label = $errinj_class/$errinj_status DELETE of expired and unexpired rows
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = REPLACE INTO tt_c VALUES (1, 1, 'rep', UNIX_TIMESTAMP()), (7, 7, 'rep', UNIX_TIMESTAMP())
--let $errinj_label = $errinj_class/$errinj_status REPLACE of an expired and an unexpired row
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO t_outt SELECT pk, a, b FROM tt_h
--let $errinj_label = $errinj_class/$errinj_status scan with TTL filtering
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO t_outt SELECT STRAIGHT_JOIN tt_c.pk, tt_c.a, tt_c.b FROM tt_h JOIN tt_c FORCE INDEX (PRIMARY) ON tt_c.pk = tt_h.a
--let $errinj_label = $errinj_class/$errinj_status point reads with TTL filtering
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_stmt = INSERT INTO tt_c SELECT pk + 100, a, b, UNIX_TIMESTAMP() FROM tt_h
--let $errinj_label = $errinj_class/$errinj_status INSERT ... SELECT from a TTL table
--source suite/rocksdb/include/errinj_loop.inc

--let $errinj_source = suite/rocksdb/include/errinj_admin.inc
--let $errinj_admin_stmt = OPTIMIZE TABLE tt_h
--let $errinj_label = $errinj_class/$errinj_status OPTIMIZE TABLE (TTL compaction, hidden TTL)
--source suite/rocksdb/include/errinj_loop.inc
--let $errinj_admin_stmt = OPTIMIZE TABLE tt_c
--let $errinj_label = $errinj_class/$errinj_status OPTIMIZE TABLE (TTL compaction, TTL column)
--source suite/rocksdb/include/errinj_loop.inc
--let $errinj_source = suite/rocksdb/include/errinj_stmt.inc

--let $errinj_stmt = ALTER TABLE tt_h FORCE, ALGORITHM=COPY
--let $errinj_label = $errinj_class/$errinj_status COPY ALTER of a TTL table
--source suite/rocksdb/include/errinj_loop.inc
