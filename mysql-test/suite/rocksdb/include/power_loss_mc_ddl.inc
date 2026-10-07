# Holds a manual compaction of column family cf_mc at $mc_point, runs one DDL
# statement on a table in that column family while it is held, crashes the
# server (a simulated power cut when the server runs with
# rocksdb_fault_injection_options={"power_loss":true}, otherwise a plain
# kill) and checks the state after recovery.
#
# $mc_point         start or middle
# $mc_ddl           the DDL statement
# $mc_delete_range  value of rocksdb_enable_delete_range_for_drop_index
#                   (optional, default 0)
# $mc_t2_keep       rows of t2 that must remain after the DDL, as a WHERE
#                   condition (optional, default TRUE); 'GONE' if the DDL
#                   drops t2
# $mc_tp_keep       the same for tp (optional, default TRUE)
#
# A DROP INDEX, DROP TABLE, TRUNCATE or partition DDL makes the drop-index
# thread compact the dropped key ranges; that compaction is held too, so the
# crash happens while dropped indexes are being reclaimed. After recovery
# every index must have the expected rows, the drop queue must drain, and a
# full compaction must leave no data of dropped indexes behind.

--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS t1, t2, tp, mc_state;
--enable_warnings
--let $_mc_mode = `SELECT @@global.rocksdb_flush_log_at_trx_commit`
SET GLOBAL rocksdb_flush_log_at_trx_commit = 1;
SET SESSION transaction_isolation = 'READ-COMMITTED';
if ($mc_delete_range) {
  SET GLOBAL rocksdb_enable_delete_range_for_drop_index = 1;
}
if (!$mc_delete_range) {
  SET GLOBAL rocksdb_enable_delete_range_for_drop_index = 0;
}
CREATE TABLE mc_state (k VARCHAR(64) PRIMARY KEY, v VARCHAR(64)) ENGINE=InnoDB;
CREATE TABLE t1 (id INT, a INT, b INT, c VARCHAR(200),
  PRIMARY KEY (id) COMMENT 'cfname=cf_mc', KEY ka (a) COMMENT 'cfname=cf_mc',
  UNIQUE KEY kb (b) COMMENT 'cfname=cf_mc') ENGINE=ROCKSDB;
CREATE TABLE t2 LIKE t1;
CREATE TABLE tp (id INT, a INT, b INT, c VARCHAR(200),
  PRIMARY KEY (id) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc',
  KEY ka (a) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc',
  KEY kb (b) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc') ENGINE=ROCKSDB
  PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (1001),
                           PARTITION p1 VALUES LESS THAN MAXVALUE);
--let $_mc_batch = 0
while ($_mc_batch < 4) {
  eval INSERT INTO t1 SELECT seq, seq % 97, seq, REPEAT(CHAR(97 + seq % 26), 100)
    FROM (WITH RECURSIVE s(seq) AS (SELECT $_mc_batch * 500 + 1 UNION ALL
          SELECT seq + 1 FROM s WHERE seq < ($_mc_batch + 1) * 500)
          SELECT seq FROM s) AS x;
  eval INSERT INTO tp SELECT * FROM t1 WHERE id > $_mc_batch * 500;
  eval INSERT INTO t2 SELECT * FROM t1 WHERE id > $_mc_batch * 500;
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  --inc $_mc_batch
}
# Expected rows of each table after the DDL, read through the primary key
# and stored for every index.
--let $_mc_t2_keep = TRUE
if ($mc_t2_keep) {
  --let $_mc_t2_keep = $mc_t2_keep
}
--let $_mc_tp_keep = TRUE
if ($mc_tp_keep) {
  --let $_mc_tp_keep = $mc_tp_keep
}
--let $_mc_sum = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b, c)))) FROM t1`
eval INSERT INTO mc_state VALUES ('t1.PRIMARY', '$_mc_sum'), ('t1.ka', '$_mc_sum'), ('t1.kb', '$_mc_sum');
if ($_mc_t2_keep != GONE) {
  --let $_mc_sum = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b, c)))) FROM t2 WHERE $_mc_t2_keep`
  eval INSERT INTO mc_state VALUES ('t2', '$_mc_sum');
}
--let $_mc_sum = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b, c)))) FROM tp WHERE $_mc_tp_keep`
eval INSERT INTO mc_state VALUES ('tp', '$_mc_sum');
eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $_mc_mode;
--enable_query_log

--echo # $mc_ddl, compaction held at $mc_point
connect (con_mc, localhost, root,,);
--disable_query_log
--source suite/rocksdb/include/mc_pause_mark.inc
eval SET GLOBAL debug = '+d,rocksdb_mc_pause_$mc_point';
--send SET GLOBAL rocksdb_compact_cf = 'cf_mc'
--enable_query_log
connection default;
--source suite/rocksdb/include/mc_pause_wait.inc
--disable_query_log
eval $mc_ddl;
# Let the drop-index thread start on the dropped indexes, if any.
SELECT SLEEP(1) INTO @mc_dummy;
--source suite/rocksdb/include/power_loss_or_kill.inc
--disable_query_log
disconnect con_mc;
SET SESSION transaction_isolation = 'READ-COMMITTED';
--enable_query_log

SELECT table_name FROM information_schema.tables
  WHERE table_schema = 'test' AND table_name IN ('t1', 't2', 'tp')
  ORDER BY table_name;

--disable_query_log
# Every index of every remaining table must have the expected rows.
--let $_mc_tables = t1 t2 tp
while ($_mc_tables) {
  --let $_mc_t = `SELECT SUBSTRING_INDEX('$_mc_tables', ' ', 1)`
  --let $_mc_tables = `SELECT IF(LOCATE(' ', '$_mc_tables'), SUBSTRING('$_mc_tables', LOCATE(' ', '$_mc_tables') + 1), '')`
  if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = '$_mc_t'`) {
    --let $_mc_exp = `SELECT v FROM mc_state WHERE k = '$_mc_t' OR k = '$_mc_t.PRIMARY'`
    --let $_mc_idx_list = `SELECT GROUP_CONCAT(DISTINCT index_name ORDER BY index_name SEPARATOR ' ') FROM information_schema.statistics WHERE table_schema = 'test' AND table_name = '$_mc_t'`
    while ($_mc_idx_list) {
      --let $_mc_idx = `SELECT SUBSTRING_INDEX('$_mc_idx_list', ' ', 1)`
      --let $_mc_idx_list = `SELECT IF(LOCATE(' ', '$_mc_idx_list'), SUBSTRING('$_mc_idx_list', LOCATE(' ', '$_mc_idx_list') + 1), '')`
      --let $_mc_got = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b, c)))) FROM $_mc_t FORCE INDEX ($_mc_idx)`
      if ($_mc_got != $_mc_exp) {
        --echo # MISMATCH $_mc_t via $_mc_idx: expected $_mc_exp, got $_mc_got
      }
    }
    --replace_column 2 # 3 #
    eval CHECK TABLE $_mc_t;
  }
}

# The drop queue must drain and a full compaction must remove all data of
# dropped indexes.
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
let $wait_condition = SELECT COUNT(*) = 0 FROM information_schema.rocksdb_global_info
  WHERE type = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
SET GLOBAL rocksdb_compact_cf = 'cf_mc';
--let $_mc_cf = `SELECT DISTINCT column_family FROM information_schema.rocksdb_ddl WHERE table_schema = 'test' AND table_name = 't1'`
--let $_mc_orphans = `SELECT COUNT(*) FROM information_schema.rocksdb_index_file_map m WHERE m.column_family = $_mc_cf AND m.index_number NOT IN (SELECT index_number FROM information_schema.rocksdb_ddl WHERE column_family = $_mc_cf)`
--enable_query_log
if ($_mc_orphans) {
  --echo # $_mc_orphans SST entries of dropped indexes left in cf_mc
}
if (!$_mc_orphans) {
  --echo # No data of dropped indexes left in cf_mc
}

--disable_query_log
DROP TABLE IF EXISTS t1, t2, tp, mc_state;
SET GLOBAL rocksdb_enable_delete_range_for_drop_index = DEFAULT;
--enable_query_log
--let $mc_delete_range =
--let $mc_t2_keep =
--let $mc_tp_keep =
