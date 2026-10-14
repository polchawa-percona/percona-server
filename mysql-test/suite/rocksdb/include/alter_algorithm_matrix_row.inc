# Runs $op with each ALGORITHM and LOCK clause through
# alter_algorithm_matrix_op.inc. $tbl, if set, is kept for every clause.

--let $_aam_keep_tbl = $tbl
--echo
--let $algo =
--source suite/rocksdb/include/alter_algorithm_matrix_op.inc
--let $tbl = $_aam_keep_tbl
--let $algo = , ALGORITHM=INSTANT
--source suite/rocksdb/include/alter_algorithm_matrix_op.inc
--let $tbl = $_aam_keep_tbl
--let $algo = , ALGORITHM=INPLACE
--source suite/rocksdb/include/alter_algorithm_matrix_op.inc
--let $tbl = $_aam_keep_tbl
--let $algo = , ALGORITHM=COPY
--source suite/rocksdb/include/alter_algorithm_matrix_op.inc
--let $tbl = $_aam_keep_tbl
--let $algo = , LOCK=NONE
--source suite/rocksdb/include/alter_algorithm_matrix_op.inc
--let $tbl = $_aam_keep_tbl
--let $algo = , LOCK=SHARED
--source suite/rocksdb/include/alter_algorithm_matrix_op.inc
--let $tbl =
