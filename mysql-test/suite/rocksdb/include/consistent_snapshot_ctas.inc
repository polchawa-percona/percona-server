# Runs the CREATE TABLE ... SELECT snapshot-window scenario on $engine.
# Expects connections con_ctas and con_reader to exist.

--connection default
eval CREATE TABLE src (id INT PRIMARY KEY, v INT) ENGINE=$engine;
INSERT INTO src VALUES (1,1),(2,2),(3,3);
eval CREATE TABLE other (id INT PRIMARY KEY) ENGINE=$engine;
INSERT INTO other VALUES (1);

# The CTAS creates t2, writes its first row and then waits, before it
# commits. The table exists from this point, its rows are not committed.
--connection con_ctas
# At REPEATABLE READ, MyRocks refuses CREATE TABLE ... SELECT with the
# gap-lock error, so the CTAS runs at READ COMMITTED on both engines. The
# reader stays at REPEATABLE READ.
SET SESSION transaction_isolation = 'READ-COMMITTED';
SET DEBUG_SYNC = 'create_select_after_write_rows_event SIGNAL ctas_created WAIT_FOR ctas_go';
--send_eval CREATE TABLE t2 ENGINE=$engine SELECT * FROM src

# The reader takes its snapshot inside the window: after t2 was created,
# before its rows were committed.
--connection con_reader
SET DEBUG_SYNC = 'now WAIT_FOR ctas_created';
START TRANSACTION WITH CONSISTENT SNAPSHOT;
SELECT COUNT(*) FROM other;
# Reading t2 waits for the CTAS's metadata lock.
--send SELECT COUNT(*) FROM t2

--connection default
let $wait_condition =
  SELECT COUNT(*) = 1 FROM information_schema.processlist
  WHERE state = 'Waiting for table metadata lock'
    AND info = 'SELECT COUNT(*) FROM t2';
--source include/wait_condition.inc
SET DEBUG_SYNC = 'now SIGNAL ctas_go';

--connection con_ctas
--reap

# The rows of t2 were committed after the reader's snapshot, so the reader
# cannot see them. It must get ER_TABLE_DEF_CHANGED, not an empty table.
--connection con_reader
--error ER_TABLE_DEF_CHANGED
--reap
COMMIT;
# A new transaction sees all rows.
SELECT COUNT(*) FROM t2;

--connection default
SET DEBUG_SYNC = 'RESET';
DROP TABLE src, other, t2;
