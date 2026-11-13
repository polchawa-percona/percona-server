# Holds a manual compaction of column family cf_mc at $mc_point, runs one DDL
# statement on a table in that column family while it is held, crashes the
# server (a simulated power cut when the server runs with
# rocksdb_debug_unsynced_data_loss=ON, otherwise a plain abort; fired by the
# crash-point hook through power_loss_now.inc, from the client session while
# the compaction thread is held) and checks the state after recovery.
#
# $mc_point            start or middle
# $mc_ddl              the DDL statement
# $mc_delete_range     value of rocksdb_enable_delete_range_for_drop_index
#                      (optional, default 0). With 1 the server is also
#                      restarted with the variable on and with the hook
#                      rocksdb_pause_drop_index_delete_range set, and after
#                      recovery the test waits for that hook, which only the
#                      delete-range path of the recovered drop-index thread
#                      reaches (a CompactRange does not).
# $mc_drop_hold        where the drop-index thread is held before the crash:
#                      before (default) it looks at the dropped indexes, or
#                      after (needs $mc_delete_range = 1) it has written the
#                      range tombstone of the first dropped index
# $mc_t2_keep          rows of t2 that must remain after the DDL, as a WHERE
#                      condition (optional, default TRUE)
# $mc_tp_keep          the same for tp (optional, default TRUE)
# $mc_t2_def           the index definitions t2 must have after the DDL,
#                      index:column:prefix-length separated by commas, the
#                      primary key first (optional; default
#                      PRIMARY:id:,ka:a:,kb:b:); GONE if the DDL drops t2
# $mc_tp_parts         the partitions tp must have after the DDL,
#                      name:upper-bound separated by commas (optional,
#                      default p0:1001,p1:MAXVALUE)
# $mc_expect_dropped   number of MyRocks indexes (of t2 and tp) that the DDL
#                      must have dropped (optional, default 0)
#
# The expected schema is given by the caller, not read back from the server,
# so a DDL statement that was acknowledged and then lost in the crash is
# caught. When the DDL drops indexes, the drop-index thread is held before it
# looks at the dropped indexes (rocksdb_pause_drop_index_thread), so the
# crash happens with the drops queued and nothing reclaimed (it does not
# compact them: a CompactRange of the drop-index thread would wait behind the
# held manual compaction), or, with delete range and $mc_drop_hold = after,
# after it has written a range tombstone and before it finished the drop.
# After recovery every index of the remaining tables must have the expected
# rows, the drop queue must drain, a full compaction must leave no data of
# the indexes that were dropped, and every remaining index must again have
# the expected rows and pass CHECK TABLE.
#
# Not covered: a hold after the compaction is installed ('completed'), a
# plain kill with rocksdb_flush_log_at_trx_commit=2 (which keeps what the OS
# has buffered).

--disable_query_log
--disable_warnings
DROP TABLE IF EXISTS t1, t2, tp, mc_state, mc_ids, mc_sync;
--enable_warnings
if ($mc_drop_hold == after) {
  if (!$mc_delete_range) {
    --die power_loss_mc_ddl.inc: $mc_drop_hold = after needs $mc_delete_range = 1
  }
}
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
CREATE TABLE mc_ids (idx INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE mc_sync (id INT PRIMARY KEY) ENGINE=ROCKSDB;
CREATE TABLE t1 (id INT, a INT, b INT, c VARCHAR(1200),
  PRIMARY KEY (id) COMMENT 'cfname=cf_mc', KEY ka (a) COMMENT 'cfname=cf_mc',
  UNIQUE KEY kb (b) COMMENT 'cfname=cf_mc') ENGINE=ROCKSDB;
CREATE TABLE t2 LIKE t1;
CREATE TABLE tp (id INT, a INT, b INT, c VARCHAR(1200),
  PRIMARY KEY (id) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc',
  KEY ka (a) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc',
  KEY kb (b) COMMENT 'p0_cfname=cf_mc;p1_cfname=cf_mc') ENGINE=ROCKSDB
  PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (1001),
                           PARTITION p1 VALUES LESS THAN MAXVALUE);
--let $_mc_batch = 0
while ($_mc_batch < 4) {
  eval INSERT INTO t1 SELECT seq, seq % 97, seq, REPEAT(CHAR(97 + seq % 26), 1000)
    FROM (WITH RECURSIVE s(seq) AS (SELECT $_mc_batch * 500 + 1 UNION ALL
          SELECT seq + 1 FROM s WHERE seq < ($_mc_batch + 1) * 500)
          SELECT seq FROM s) AS x;
  eval INSERT INTO tp SELECT * FROM t1 WHERE id > $_mc_batch * 500;
  eval INSERT INTO t2 SELECT * FROM t1 WHERE id > $_mc_batch * 500;
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  --inc $_mc_batch
}

# The expected schema after the DDL.
--let $_mc_t2_def = PRIMARY:id:,ka:a:,kb:b:
if ($mc_t2_def) {
  --let $_mc_t2_def = $mc_t2_def
}
--let $_mc_tp_parts = p0:1001,p1:MAXVALUE
if ($mc_tp_parts) {
  --let $_mc_tp_parts = $mc_tp_parts
}
--let $_mc_expect_dropped = 0
if ($mc_expect_dropped) {
  --let $_mc_expect_dropped = $mc_expect_dropped
}
# The index numbers that exist before the DDL.
INSERT INTO mc_ids SELECT DISTINCT index_number FROM information_schema.rocksdb_ddl
  WHERE table_schema = 'test' AND table_name IN ('t2', 'tp');

# Expected rows of each table after the DDL, as each index must show them:
# read through the primary key and stored for every index.
--let $_mc_t2_keep = TRUE
if ($mc_t2_keep) {
  --let $_mc_t2_keep = $mc_t2_keep
}
--let $_mc_tp_keep = TRUE
if ($mc_tp_keep) {
  --let $_mc_tp_keep = $mc_tp_keep
}
--let $mc_compare =
--let $mc_store_keep =
--let $mc_table = t1
--let $mc_key = t1
--let $mc_indexes = PRIMARY ka kb
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_table = t2
--let $mc_key = t2
--let $mc_indexes = PRIMARY ka kb kc
--let $mc_store_keep = $_mc_t2_keep
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_table = tp
--let $mc_key = tp
--let $mc_indexes = PRIMARY ka kb
--let $mc_store_keep = $_mc_tp_keep
--source suite/rocksdb/include/mc_checksum.inc
--let $mc_store_keep =
eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $_mc_mode;
--enable_query_log

--echo # $mc_ddl, compaction held at $mc_point
connect (con_mc, localhost, root,,);
--disable_query_log
--let $debug_pause_hook = rocksdb_pause_manual_compaction_$mc_point
--source suite/rocksdb/include/debug_pause_mark.inc
--let $mc_output_mode = mark
--source suite/rocksdb/include/mc_output.inc
eval SET GLOBAL debug = '+d,rocksdb_pause_manual_compaction_$mc_point';
--send SET GLOBAL rocksdb_compact_cf = 'cf_mc'
--enable_query_log
connection default;
--let $debug_pause_hook = rocksdb_pause_manual_compaction_$mc_point
--let $debug_pause_timeout = 300
--source suite/rocksdb/include/debug_pause_wait.inc
--let $mc_output_mode = check
--source suite/rocksdb/include/mc_output.inc
--disable_query_log
if ($_mc_expect_dropped) {
  # Hold the drop-index thread, so that it reclaims nothing (or, after the
  # range tombstone, so that it has not finished the drop).
  --let $_mc_hold = rocksdb_pause_drop_index_thread
  if ($mc_drop_hold == after) {
    --let $_mc_hold = rocksdb_pause_drop_index_delete_range
  }
  --let $debug_pause_hook = $_mc_hold
  --source suite/rocksdb/include/debug_pause_mark.inc
  eval SET GLOBAL debug = '+d,$_mc_hold';
}
--disable_warnings
eval $mc_ddl;
--enable_warnings
if ($_mc_expect_dropped) {
  SET GLOBAL rocksdb_signal_drop_index_thread = 1;
  --let $debug_pause_hook = $_mc_hold
  --let $debug_pause_timeout = 60
  --source suite/rocksdb/include/debug_pause_wait.inc
  --let $_mc_pending = `SELECT COUNT(*) FROM information_schema.rocksdb_global_info WHERE type = 'DDL_DROP_INDEX_ONGOING'`
  if ($_mc_pending == $_mc_expect_dropped) {
    --echo # $_mc_pending dropped indexes pending, drop-index thread held
  }
  if ($_mc_pending != $_mc_expect_dropped) {
    --echo # UNEXPECTED pending drops: $_mc_pending, expected $_mc_expect_dropped
  }
  if ($mc_drop_hold == after) {
    --echo # The drop-index thread is held after it wrote the range tombstone of the first dropped index
  }
}
--disable_query_log
if ($mc_drop_hold == after) {
  # The range tombstone was written without a sync. Commit a write with
  # rocksdb_flush_log_at_trx_commit=1 (the WAL is shared by the column
  # families), so the tombstone is durable and a power cut keeps it.
  SET GLOBAL rocksdb_flush_log_at_trx_commit = 1;
  INSERT INTO mc_sync VALUES (1);
  eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $_mc_mode;
}
if ($mc_delete_range) {
  # The recovered server must use delete range for the drops from the start,
  # and must stop its drop-index thread after the range tombstone.
  --let $restart_parameters = restart: --rocksdb_enable_delete_range_for_drop_index=1 --debug=+d,rocksdb_pause_drop_index_delete_range
  --let $debug_pause_hook = rocksdb_pause_drop_index_delete_range
  --source suite/rocksdb/include/debug_pause_mark.inc
}
--enable_query_log
--source suite/rocksdb/include/power_loss_now.inc
--disable_query_log
--let $restart_parameters =
disconnect con_mc;
SET SESSION transaction_isolation = 'READ-COMMITTED';
if ($mc_delete_range) {
  SET GLOBAL rocksdb_signal_drop_index_thread = 1;
  --let $debug_pause_hook = rocksdb_pause_drop_index_delete_range
  --let $debug_pause_timeout = 300
  --source suite/rocksdb/include/debug_pause_wait.inc
  --let $_mc_dr = `SELECT @@global.rocksdb_enable_delete_range_for_drop_index`
  --echo # Recovered with rocksdb_enable_delete_range_for_drop_index=$_mc_dr: the drop-index thread wrote a range tombstone
}
--enable_query_log

# The schema the DDL left must have survived.
--disable_query_log
--let $_mc_got = `SELECT COALESCE(GROUP_CONCAT(CONCAT(index_name, ':', column_name, ':', COALESCE(sub_part, '')) ORDER BY index_name <> 'PRIMARY', index_name, seq_in_index SEPARATOR ','), 'GONE') FROM information_schema.statistics WHERE table_schema = 'test' AND table_name = 't2'`
if ($_mc_got != $_mc_t2_def) {
  --echo # SCHEMA MISMATCH t2: expected $_mc_t2_def, got $_mc_got
}
--let $_mc_got = `SELECT GROUP_CONCAT(CONCAT(index_name, ':', column_name, ':', COALESCE(sub_part, '')) ORDER BY index_name <> 'PRIMARY', index_name, seq_in_index SEPARATOR ',') FROM information_schema.statistics WHERE table_schema = 'test' AND table_name = 'tp'`
if ($_mc_got != 'PRIMARY:id:,ka:a:,kb:b:') {
  --echo # SCHEMA MISMATCH tp indexes: got $_mc_got
}
--let $_mc_got = `SELECT GROUP_CONCAT(CONCAT(partition_name, ':', partition_description) ORDER BY partition_ordinal_position SEPARATOR ',') FROM information_schema.partitions WHERE table_schema = 'test' AND table_name = 'tp'`
if ($_mc_got != $_mc_tp_parts) {
  --echo # SCHEMA MISMATCH tp partitions: expected $_mc_tp_parts, got $_mc_got
}
# The number of indexes that are gone from MyRocks.
--let $_mc_gone = `SELECT COUNT(*) FROM mc_ids WHERE idx NOT IN (SELECT index_number FROM information_schema.rocksdb_ddl)`
if ($_mc_gone != $_mc_expect_dropped) {
  --echo # SCHEMA MISMATCH: $_mc_gone indexes dropped, expected $_mc_expect_dropped
}
--enable_query_log

SELECT table_name FROM information_schema.tables
  WHERE table_schema = 'test' AND table_name IN ('t1', 't2', 'tp')
  ORDER BY table_name;

# Every index of every remaining table must have the expected rows.
--source suite/rocksdb/include/mc_check_tables.inc

if ($mc_delete_range) {
  # Release the drop-index thread that the hook holds.
  --disable_query_log
  SET GLOBAL debug = '-d,rocksdb_pause_drop_index_delete_range';
  --enable_query_log
}
# The drop queue must drain and a full compaction must remove all data of
# dropped indexes.
--source suite/rocksdb/include/mc_drain_drops.inc
--disable_query_log
SET GLOBAL rocksdb_compact_cf = 'cf_mc';
--enable_query_log
--echo # After the drops and the full compaction every remaining index has its rows
--source suite/rocksdb/include/mc_check_tables.inc
--disable_query_log
--let $_mc_cf = `SELECT DISTINCT column_family FROM information_schema.rocksdb_ddl WHERE table_schema = 'test' AND table_name = 't1'`
# Data of the indexes dropped by the DDL...
--let $_mc_left = `SELECT COUNT(*) FROM information_schema.rocksdb_index_file_map m WHERE m.column_family = $_mc_cf AND m.index_number IN (SELECT idx FROM mc_ids) AND m.index_number NOT IN (SELECT index_number FROM information_schema.rocksdb_ddl)`
# ... and of any other index that is not defined.
--let $_mc_orphans = `SELECT COUNT(*) FROM information_schema.rocksdb_index_file_map m WHERE m.column_family = $_mc_cf AND m.index_number NOT IN (SELECT index_number FROM information_schema.rocksdb_ddl WHERE column_family = $_mc_cf)`
--enable_query_log
if ($_mc_left) {
  --echo # $_mc_left SST entries of the indexes dropped by the DDL left in cf_mc
}
if ($_mc_orphans) {
  --echo # $_mc_orphans SST entries of dropped indexes left in cf_mc
}
if (!$_mc_orphans) {
  --echo # No data of dropped indexes left in cf_mc
}

--disable_query_log
DROP TABLE IF EXISTS t1, t2, tp, mc_state, mc_ids, mc_sync;
SET GLOBAL rocksdb_enable_delete_range_for_drop_index = DEFAULT;
--enable_query_log
--source suite/rocksdb/include/mc_drain_drops.inc
--let $mc_delete_range =
--let $mc_drop_hold =
--let $mc_t2_keep =
--let $mc_tp_keep =
--let $mc_t2_def =
--let $mc_tp_parts =
--let $mc_expect_dropped =
--let $mc_compare =
