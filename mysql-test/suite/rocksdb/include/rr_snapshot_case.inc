# Runs one write-validation case of rr_snapshot_write_conflict.test.
#
# con1 takes a REPEATABLE READ snapshot with a plain read, con2 then commits
# $rr_change (autocommit), and con1 runs $rr_stmt against rows that changed
# after its snapshot. Prints the outcome and rolls con1 back, then restores
# the table contents.
#
# Parameters: $rr_change, $rr_stmt.

--connection con1
BEGIN;
SELECT COUNT(*) FROM t;
--connection con2
--eval $rr_change
--connection con1
--error 0,ER_LOCK_DEADLOCK,ER_DUP_ENTRY,ER_LOCK_WAIT_TIMEOUT
--eval $rr_stmt
--source suite/rocksdb/include/rr_snapshot_outcome.inc
ROLLBACK;
--connection default
DELETE FROM t;
INSERT INTO t VALUES (1,10,1,1),(2,20,2,2),(3,30,3,3),(4,40,4,4),(5,50,5,5);
