# Holds a manual compaction at $mc_point, commits DML transactions while it is
# held, crashes the server (a simulated power cut when the server runs with
# rocksdb_fault_injection_options={"power_loss":true}, otherwise a plain
# kill) and checks the state after recovery.
#
# $mc_trigger  statement that starts the manual compaction (run in con_mc)
# $mc_setup    statement run in con_mc before $mc_trigger (optional)
# $mc_point    start, middle or completed
# $mc_flush    1 to flush the memtables while the compaction is held
#
# All tables are in column family cf_mc, which has automatic compactions
# disabled, so the only compaction is the manual one. t1 and tp are the
# compacted tables; t2 shares their column family and gets the DML. Each DML
# transaction is also replayed on the InnoDB table t2ref after recovery, so
# the state of t2 can be checked for any number of recovered transactions.
# With rocksdb_flush_log_at_trx_commit=1 every acknowledged transaction must
# be recovered; with 2, a prefix of them.

--let $_mc_n = 20
--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS t1, t2, t2ref, tp, mc_state;
--enable_warnings
--let $_mc_mode = `SELECT @@global.rocksdb_flush_log_at_trx_commit`
SET GLOBAL rocksdb_flush_log_at_trx_commit = 1;
# INSERT ... SELECT would need a gap lock at REPEATABLE READ.
SET SESSION transaction_isolation = 'READ-COMMITTED';
CREATE TABLE mc_state (k VARCHAR(64) PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB;
CREATE TABLE t1 (id INT, a INT, b INT, c VARCHAR(200),
  PRIMARY KEY (id) COMMENT 'cfname=cf_mc', KEY ka (a) COMMENT 'cfname=cf_mc',
  UNIQUE KEY kb (b) COMMENT 'cfname=cf_mc') ENGINE=ROCKSDB;
CREATE TABLE t2 LIKE t1;
CREATE TABLE tp (id INT, a INT, b INT, c VARCHAR(200),
  PRIMARY KEY (id) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc',
  KEY ka (a) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc',
  KEY kb (b) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc') ENGINE=ROCKSDB
  PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (2001),
                           PARTITION p1 VALUES LESS THAN MAXVALUE);
CREATE TABLE t2ref (id INT PRIMARY KEY, a INT, b INT, c VARCHAR(200),
  KEY ka (a), UNIQUE KEY kb (b)) ENGINE=InnoDB;
# Four flushed batches per table give four overlapping L0 files.
--let $_mc_batch = 0
while ($_mc_batch < 4) {
  eval INSERT INTO t1 SELECT seq, seq % 97, seq, REPEAT(CHAR(97 + seq % 26), 100)
    FROM (WITH RECURSIVE s(seq) AS (SELECT $_mc_batch * 1000 + 1 UNION ALL
          SELECT seq + 1 FROM s WHERE seq < ($_mc_batch + 1) * 1000)
          SELECT seq FROM s) AS x;
  eval INSERT INTO tp SELECT * FROM t1 WHERE id > $_mc_batch * 1000;
  eval INSERT INTO t2 SELECT * FROM t1 WHERE id > $_mc_batch * 1000 AND id <= $_mc_batch * 1000 + 500;
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  --inc $_mc_batch
}
INSERT INTO t2ref SELECT * FROM t2;
--let $mc_compare =
--let $mc_indexes = PRIMARY ka kb
--let $mc_table = t1
--let $mc_key = t1
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_table = tp
--let $mc_key = tp
--source suite/rocksdb/include/mc_checksum.inc
eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $_mc_mode;
--enable_query_log

--echo # $mc_trigger, held at $mc_point
connect (con_mc, localhost, root,,);
--disable_query_log
if ($mc_setup) {
  eval $mc_setup;
}
--source suite/rocksdb/include/mc_pause_mark.inc
eval SET GLOBAL debug = '+d,rocksdb_mc_pause_$mc_point';
--send_eval $mc_trigger
--enable_query_log
connection default;
--source suite/rocksdb/include/mc_pause_wait.inc

--disable_query_log
--let $_mc_i = 1
while ($_mc_i <= $_mc_n) {
  START TRANSACTION;
  eval INSERT INTO t2 VALUES (10000 + $_mc_i, $_mc_i, 100000 + $_mc_i, REPEAT('z', 100));
  eval UPDATE t2 SET a = a + 1, b = b + 200000 WHERE id = $_mc_i * 7 % 2000 + 1;
  eval DELETE FROM t2 WHERE id = $_mc_i * 13 % 2000 + 1;
  COMMIT;
  if ($mc_flush) {
    if ($_mc_i == 10) {
      SET GLOBAL rocksdb_force_flush_memtable_now = 1;
    }
  }
  --inc $_mc_i
}
--source suite/rocksdb/include/power_loss_or_kill.inc
--disable_query_log
disconnect con_mc;
SET SESSION transaction_isolation = 'READ-COMMITTED';

# Number of recovered DML transactions: the inserted ids are consecutive.
--let $_mc_j = `SELECT COALESCE(MAX(id), 10000) - 10000 FROM t2`
--let $_mc_ok = `SELECT IF(@@global.rocksdb_flush_log_at_trx_commit = 1, $_mc_j = $_mc_n, $_mc_j <= $_mc_n)`
--let $_mc_i = 1
while ($_mc_i <= $_mc_j) {
  eval INSERT INTO t2ref VALUES (10000 + $_mc_i, $_mc_i, 100000 + $_mc_i, REPEAT('z', 100));
  eval UPDATE t2ref SET a = a + 1, b = b + 200000 WHERE id = $_mc_i * 7 % 2000 + 1;
  eval DELETE FROM t2ref WHERE id = $_mc_i * 13 % 2000 + 1;
  --inc $_mc_i
}
--let $mc_compare =
--let $mc_table = t2ref
--let $mc_key = t2
--let $mc_indexes = PRIMARY
--source suite/rocksdb/include/mc_checksum.inc
DELETE FROM mc_state WHERE k = 't2.ka' OR k = 't2.kb';
INSERT INTO mc_state SELECT 't2.ka', v FROM mc_state WHERE k = 't2.PRIMARY';
INSERT INTO mc_state SELECT 't2.kb', v FROM mc_state WHERE k = 't2.PRIMARY';
--enable_query_log
if ($_mc_ok) {
  --echo # Recovered DML transactions: as expected
}
if (!$_mc_ok) {
  --echo # Recovered DML transactions: $_mc_j of $_mc_n
}
--let $mc_compare = 1
--let $mc_indexes = PRIMARY ka kb
--let $mc_table = t1
--let $mc_key = t1
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_table = tp
--let $mc_key = tp
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_table = t2
--let $mc_key = t2
--source suite/rocksdb/include/mc_checksum.inc
--replace_column 2 # 3 #
CHECK TABLE t1, t2, tp;

--echo # The same compaction again, run to the end
--disable_query_log
--disable_result_log
if ($mc_setup) {
  eval $mc_setup;
}
eval $mc_trigger;
--enable_result_log
--enable_query_log
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_table = t1
--let $mc_key = t1
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_table = tp
--let $mc_key = tp
--source suite/rocksdb/include/mc_checksum.inc

--disable_query_log
DROP TABLE t1, t2, t2ref, tp, mc_state;
--enable_query_log
--let $mc_setup =
--let $mc_flush =
