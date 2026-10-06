# Concurrent COPY and INSTANT ALTER TABLE on MyRocks.
#
# All tables live in the default column family, so the SST files written by
# one ALTER also hold the rows of the other tables, the rows copied by the
# other ALTERs and the indexes they drop.  While $ntab sessions rebuild
# tables in a loop (INSTANT ADD COLUMN, COPY DROP COLUMN, COPY FORCE), two
# sessions update other tables, one session reads t1 through the primary and
# a secondary key, and one session holds a consistent snapshot of t6.
# Optionally ($chaos) another session keeps flushing memtables, compacting,
# waking the drop-index thread and pausing background work.
#
# Checks: every table keeps its rows and its secondary indexes agree with
# the primary key, no read returns a partial table, every update is kept,
# the old snapshot stays unchanged, no #sql table is left behind, the
# dropped index sets are reclaimed and no SST data of a dropped index
# remains, and a restart with table validation is clean.
#
# Parameters:
#   $ntab         number of altering sessions (tables t1..t$ntab)
#   $nrows        rows per table
#   $iters        loop iterations per altering session
#   $same_table   if set, all altering sessions alter t1
#   $alter_setup  statement run in each altering session first
#   $chaos        if set, run the background-work session
#   $chaos_iters  its iterations (about 0.5 s each)
#   $chaos_pause  1 to also pause background work for 2 s at times

--let $save_instant_append = `SELECT @@global.rocksdb_enable_instant_ddl_for_append_column`
SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = ON;
--disable_query_log
SET SESSION cte_max_recursion_depth = 1000000;
let $i= 1;
while ($i <= 6) {
  eval CREATE TABLE t$i (id INT PRIMARY KEY, a INT, b VARCHAR(64), c INT, KEY ka(a), KEY kb(b)) ENGINE=ROCKSDB;
  eval INSERT INTO t$i WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM s WHERE n < $nrows) SELECT n, n % 997, CONCAT('v', n), n FROM s;
  inc $i;
}
--enable_query_log
DELIMITER |;
CREATE PROCEDURE alter_loop(tn VARCHAR(10), iters INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < iters DO
    SET @s = CONCAT('ALTER TABLE ', tn, ' ADD COLUMN x', CONNECTION_ID(), '_', i, ' INT DEFAULT ', i, ', ALGORITHM=INSTANT');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
    SET @s = CONCAT('ALTER TABLE ', tn, ' DROP COLUMN x', CONNECTION_ID(), '_', i, ', ALGORITHM=COPY');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
    SET @s = CONCAT('ALTER TABLE ', tn, ' FORCE, ALGORITHM=COPY');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
    SET i = i + 1;
  END WHILE;
END|
CREATE PROCEDURE dml_loop(tn VARCHAR(10), iters INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < iters DO
    SET @s = CONCAT('UPDATE ', tn, ' SET c = c + 1 WHERE id = ', 1 + (i * 37) % 20000);
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
    SET i = i + 1;
  END WHILE;
END|
CREATE PROCEDURE read_loop(tn VARCHAR(10), iters INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  DECLARE CONTINUE HANDLER FOR 1412 SET @def_changed = IFNULL(@def_changed,0) + 1;
  CREATE TEMPORARY TABLE IF NOT EXISTS rl_bad (it INT, cnt BIGINT, cnt_a BIGINT) ENGINE=InnoDB;
  WHILE i < iters DO
    SET @s = CONCAT('SELECT COUNT(*), SUM(id) INTO @cnt, @sid FROM ', tn);
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
    SET @s = CONCAT('SELECT COUNT(*) INTO @cnta FROM ', tn, ' FORCE INDEX(ka)');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
    IF @cnt <> 20000 OR @cnta <> 20000 OR @sid <> 200010000 THEN
      INSERT INTO rl_bad VALUES (i, @cnt, @cnta);
    END IF;
    SET i = i + 1;
  END WHILE;
  SELECT COUNT(*) AS bad_reads FROM rl_bad;
  DROP TEMPORARY TABLE rl_bad;
END|
CREATE PROCEDURE chaos_loop(iters INT, do_pause INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < iters DO
    SET GLOBAL rocksdb_force_flush_memtable_now = ON;
    SET GLOBAL rocksdb_signal_drop_index_thread = ON;
    IF i % 3 = 0 THEN SET GLOBAL rocksdb_compact_cf = 'default'; END IF;
    IF do_pause = 1 AND i % 4 = 1 THEN
      SET GLOBAL rocksdb_pause_background_work = ON;
      DO SLEEP(2);
      SET GLOBAL rocksdb_pause_background_work = OFF;
    END IF;
    DO SLEEP(0.5);
    SET i = i + 1;
  END WHILE;
END|
DELIMITER ;|

let $c= 1;
while ($c <= $ntab) {
  connect (ca$c,localhost,root,,);
  if ($alter_setup) {
    eval $alter_setup;
  }
  if ($same_table) {
    --send_eval CALL alter_loop('t1', $iters)
  }
  if (!$same_table) {
    --send_eval CALL alter_loop('t$c', $iters)
  }
  inc $c;
}
connect (cd5,localhost,root,,);
--send CALL dml_loop('t5', 3000)
connect (cd6,localhost,root,,);
--send CALL dml_loop('t6', 3000)
connect (cr1,localhost,root,,);
--send CALL read_loop('t1', 400)
if ($chaos) {
  connect (cx1,localhost,root,,);
  --send_eval CALL chaos_loop($chaos_iters, $chaos_pause)
}
connect (csnap,localhost,root,,);
START TRANSACTION WITH CONSISTENT SNAPSHOT;
SELECT SUM(c) INTO @snap_c FROM t6;
connection default;
connection default;
let $c= 1;
while ($c <= $ntab) {
  connection ca$c;
  --reap
  disconnect ca$c;
  inc $c;
}
connection cd5;
--reap
disconnect cd5;
connection cd6;
--reap
disconnect cd6;
connection cr1;
--reap
disconnect cr1;
if ($chaos) {
  connection cx1;
  --reap
  disconnect cx1;
}
connection csnap;
SELECT SUM(c) = @snap_c AS old_snapshot_of_t6_unchanged FROM t6;
COMMIT;
disconnect csnap;
connection default;
connection default;
--echo # Checks
let $i= 1;
while ($i <= 6) {
  eval SELECT '$i' t, COUNT(*), SUM(id), SUM(a), (SELECT COUNT(*) FROM t$i FORCE INDEX(ka)) ka, (SELECT COUNT(*) FROM t$i FORCE INDEX(kb)) kb FROM t$i;
  inc $i;
}
SELECT SUM(c) - 200010000 AS t5_updates FROM t5;
SELECT SUM(c) - 200010000 AS t6_updates FROM t6;
SELECT TABLE_NAME FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME LIKE '#sql%';
SET GLOBAL rocksdb_signal_drop_index_thread = ON;
let $wait_timeout= 300;
let $wait_condition= SELECT COUNT(*) = 0 FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
SELECT COUNT(*) AS drop_ongoing FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'DDL_DROP_INDEX_ONGOING';
SET GLOBAL rocksdb_force_flush_memtable_now = ON;
SET GLOBAL rocksdb_compact_cf = 'default';
SELECT COUNT(*) AS orphan_sst_index_entries FROM (SELECT DISTINCT INDEX_NUMBER FROM information_schema.ROCKSDB_INDEX_FILE_MAP WHERE COLUMN_FAMILY = 0) m WHERE INDEX_NUMBER NOT IN (SELECT INDEX_NUMBER FROM information_schema.ROCKSDB_DDL);
SET GLOBAL rocksdb_signal_drop_index_thread = ON;
--source include/restart_mysqld.inc
let $i= 1;
while ($i <= 6) {
  eval SELECT '$i' t, COUNT(*), SUM(id) FROM t$i;
  eval CHECK TABLE t$i;
  inc $i;
}
DROP PROCEDURE alter_loop;
DROP PROCEDURE dml_loop;
DROP PROCEDURE read_loop;
DROP PROCEDURE chaos_loop;
SET GLOBAL rocksdb_pause_background_work = OFF;
DROP TABLE t1, t2, t3, t4, t5, t6;
--disable_query_log
--eval SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = $save_instant_append
--enable_query_log
--source include/wait_until_count_sessions.inc
