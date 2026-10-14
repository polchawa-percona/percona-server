# ==== Purpose ====
#
# Check the MyRocks invariants that hold after any scenario, so that a test
# also catches silent corruption it was not written for (CHECK TABLE said OK
# while index entries were missing in PS-11744, PS-11772 and PS-11696).
#
#   1. Every secondary index has the same row count and checksum as the
#      primary key, and CHECK TABLE reports OK.
#   3. The MyRocks dictionary matches the server data dictionary: every
#      index of every ROCKSDB table has exactly one ROCKSDB_DDL entry, every
#      ROCKSDB_DDL entry belongs to an existing table and index, no index
#      number is used twice, and no index number exceeds MAX_INDEX_ID.
#   4. The drop-index queue drains, and afterwards no SST file has rows
#      for an index number that is no longer in the dictionary.
#
# Invariants 2 (InnoDB mirror), 5 (snapshot across compaction), 6 (crash
# loses only a suffix) and 7 (errors are truthful) need scenario context;
# see myrocks_inv_mirror_compare.inc, myrocks_inv_snapshot_begin.inc /
# myrocks_inv_snapshot_end.inc, myrocks_inv_crash_suffix.inc and
# myrocks_inv_stmt.inc.
#
# The include prints nothing when every check passes. Each violation is
# printed as a row starting with "INVARIANT VIOLATION", so it shows up as a
# result difference.
#
# Run it when the server is quiescent: no open transactions that hold
# uncommitted changes, no DDL in progress.
#
# ==== Usage ====
#
# [--let $inv_schema = db1]          # default: DATABASE()
# [--let $inv_skip_check_table = 1]  # skip CHECK TABLE
# [--let $inv_skip_dropped = 1]      # skip invariant 4
# [--let $inv_drop_timeout = 300]    # seconds to wait for the drop queue
# [--let $inv_die = 1]               # --die on the first violation found
# --source suite/rocksdb/include/myrocks_invariants.inc
#
# CHECK TABLE is skipped for tables whose name contains a comma (mysqltest
# cannot run it for them); $inv_check_skipped returns how many were skipped.
# Invariant 3 skips tables whose name needs file-name encoding;
# $inv_dict_skipped returns how many were skipped.
#
# Invisible indexes and indexes with a partial_group comment (partial
# indexes) are skipped by invariant 1. A prefix index cannot return rows in
# column order, so its side of the comparison may fall back to a table scan
# and only CHECK TABLE checks it.
# Functional key parts are not checksummed; their row count still is.

--disable_query_log
--disable_result_log

--let $_inv_schema = $inv_schema
if (!$_inv_schema) {
  --let $_inv_schema = `SELECT DATABASE()`
}
--let $_inv_violations = 0

SET @inv_saved_gcml = @@SESSION.group_concat_max_len;
SET SESSION group_concat_max_len = 16 * 1024 * 1024;
# INSERT ... SELECT from MyRocks tables needs gap locks at REPEATABLE READ.
SET @inv_saved_iso = @@SESSION.transaction_isolation;
SET SESSION transaction_isolation = 'READ-COMMITTED';
--eval SET @inv_schema = '$_inv_schema'

# Open every MyRocks table of the schema first: until a table is opened
# after a restart, ROCKSDB_DDL shows empty index names for it (PS-11882),
# which invariant 3 would report as a mismatch.
CREATE TEMPORARY TABLE inv_tabs ENGINE=InnoDB AS
SELECT TABLE_NAME AS tn, ROW_NUMBER() OVER (ORDER BY TABLE_NAME) AS n
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = @inv_schema AND ENGINE = 'ROCKSDB';
--let $_inv_ntabs = `SELECT COUNT(*) FROM inv_tabs`
--let $_inv_i = 1
while ($_inv_i <= $_inv_ntabs) {
  --let $_inv_tn = `SELECT REPLACE(tn, CHAR(96), REPEAT(CHAR(96), 2)) FROM inv_tabs WHERE n = $_inv_i`
  --eval SELECT 1 FROM `$_inv_schema`.`$_inv_tn` LIMIT 0
  --inc $_inv_i
}

#
# Invariant 1: secondary index vs primary key, per table and index.
#
# Each secondary index is read in index order with FORCE INDEX, which makes
# the optimizer scan the index; the primary side reads the table with no
# index allowed, which is a primary-key scan in MyRocks. Both sides
# checksum the index columns plus the primary key columns.
#
CREATE TEMPORARY TABLE inv_cols ENGINE=InnoDB AS
SELECT s.TABLE_NAME AS tn, s.INDEX_NAME AS idx,
       GROUP_CONCAT(DISTINCT CONCAT('`', REPLACE(s.COLUMN_NAME, '`', '``'), '`')
                    ORDER BY s.SEQ_IN_INDEX) AS cols
  FROM information_schema.STATISTICS s
  JOIN information_schema.TABLES t
    ON t.TABLE_SCHEMA = s.TABLE_SCHEMA AND t.TABLE_NAME = s.TABLE_NAME
 WHERE s.TABLE_SCHEMA = @inv_schema AND t.ENGINE = 'ROCKSDB'
   AND s.COLUMN_NAME IS NOT NULL
 GROUP BY s.TABLE_NAME, s.INDEX_NAME;

CREATE TEMPORARY TABLE inv_sk ENGINE=InnoDB AS
SELECT DISTINCT s.TABLE_NAME AS tn, s.INDEX_NAME AS idx
  FROM information_schema.STATISTICS s
  JOIN information_schema.TABLES t
    ON t.TABLE_SCHEMA = s.TABLE_SCHEMA AND t.TABLE_NAME = s.TABLE_NAME
 WHERE s.TABLE_SCHEMA = @inv_schema AND t.ENGINE = 'ROCKSDB'
   AND s.INDEX_NAME <> 'PRIMARY' AND s.IS_VISIBLE = 'YES'
   AND s.INDEX_COMMENT NOT LIKE '%partial_group%';

# Columns of the index plus the primary key columns, each once, as a
# select list and as a checksum expression. Identifiers are quoted with
# backticks (doubled inside), so any column name works.
CREATE TEMPORARY TABLE inv_allcols ENGINE=InnoDB AS
SELECT k.tn, k.idx,
       COALESCE(GROUP_CONCAT(DISTINCT CONCAT('`', REPLACE(s.COLUMN_NAME, '`', '``'), '`')
                             ORDER BY s.COLUMN_NAME), '1') AS cols,
       COALESCE(GROUP_CONCAT(DISTINCT CONCAT('IFNULL(HEX(CAST(`',
                                             REPLACE(s.COLUMN_NAME, '`', '``'),
                                             '` AS BINARY)),''N'')')
                             ORDER BY s.COLUMN_NAME), '1') AS expr
  FROM inv_sk k
  LEFT JOIN information_schema.STATISTICS s
    ON s.TABLE_SCHEMA = @inv_schema AND s.TABLE_NAME = k.tn
   AND s.INDEX_NAME IN (k.idx, 'PRIMARY') AND s.COLUMN_NAME IS NOT NULL
 GROUP BY k.tn, k.idx;

# One row per secondary index: row counts and checksums of both sides.
SET @inv_q = NULL;
SET @inv_qs = REPLACE(@inv_schema, '`', '``');
SELECT GROUP_CONCAT(
  CONCAT(
    'SELECT ', QUOTE(k.tn), ' AS tn, ', QUOTE(k.idx), ' AS idx, ',
    '(SELECT COUNT(*) FROM `', @inv_qs, '`.`', k.qtn, '` USE INDEX ()) AS pk_rows, ',
    '(SELECT COUNT(*) FROM (SELECT 1 FROM `', @inv_qs, '`.`', k.qtn,
      '` FORCE INDEX (`', k.qidx, '`) ORDER BY ', ic.cols,
      ' LIMIT 18446744073709551615) d) AS sk_rows, ',
    '(SELECT SUM(CRC32(CONCAT_WS(''#'',', ac.expr, '))) FROM `', @inv_qs, '`.`', k.qtn,
      '` USE INDEX ()) AS pk_sum, ',
    '(SELECT SUM(CRC32(CONCAT_WS(''#'',', ac.expr, '))) FROM (SELECT ', ac.cols, ' FROM `',
      @inv_qs, '`.`', k.qtn, '` FORCE INDEX (`', k.qidx, '`) ORDER BY ', ic.cols,
      ' LIMIT 18446744073709551615) d) AS sk_sum')
  SEPARATOR ' UNION ALL ') INTO @inv_q
  FROM (SELECT tn, idx, REPLACE(tn, '`', '``') AS qtn, REPLACE(idx, '`', '``') AS qidx
          FROM inv_sk) k
  JOIN inv_cols ic ON ic.tn = k.tn AND ic.idx = k.idx
  JOIN inv_allcols ac ON ac.tn = k.tn AND ac.idx = k.idx;

--let $_inv_have_sk = `SELECT @inv_q IS NOT NULL`
if ($_inv_have_sk) {
  SET @inv_q = CONCAT('SELECT ''INVARIANT VIOLATION 1: secondary index differs from primary key'' AS v, x.* FROM (',
                      @inv_q, ') x WHERE NOT (x.pk_rows <=> x.sk_rows) OR NOT (x.pk_sum <=> x.sk_sum)');
  CREATE TEMPORARY TABLE inv_v1 (v VARCHAR(100), tn VARCHAR(64), idx VARCHAR(64),
                                 pk_rows BIGINT, sk_rows BIGINT,
                                 pk_sum DECIMAL(65), sk_sum DECIMAL(65)) ENGINE=InnoDB;
  SET @inv_q = CONCAT('INSERT INTO inv_v1 ', @inv_q);
  PREPARE inv_stmt FROM @inv_q;
  EXECUTE inv_stmt;
  DEALLOCATE PREPARE inv_stmt;
  --let $_inv_n = `SELECT COUNT(*) FROM inv_v1`
  if ($_inv_n) {
    --enable_result_log
    SELECT * FROM inv_v1 ORDER BY tn, idx;
    --disable_result_log
    --inc $_inv_violations
  }
  DROP TEMPORARY TABLE inv_v1;
}

# CHECK TABLE on every ROCKSDB table of the schema.
--let $inv_check_skipped = 0
if (!$inv_skip_check_table) {
  --let $_inv_i = 1
  while ($_inv_i <= $_inv_ntabs) {
    # query_get_value() splits its arguments at commas, and CHECK TABLE
    # cannot be prepared, so tables whose name contains a comma are not
    # checked here; $inv_check_skipped counts them.
    --let $_inv_comma = `SELECT LOCATE(',', tn) > 0 FROM inv_tabs WHERE n = $_inv_i`
    --let $_inv_ct = OK
    if ($_inv_comma) {
      --inc $inv_check_skipped
    }
    if (!$_inv_comma) {
      --let $_inv_tn = `SELECT REPLACE(tn, CHAR(96), REPEAT(CHAR(96), 2)) FROM inv_tabs WHERE n = $_inv_i`
      --let $_inv_ct = query_get_value(CHECK TABLE `$_inv_schema`.`$_inv_tn`, Msg_text, 1)
    }
    if ($_inv_ct != OK) {
      --enable_result_log
      --echo INVARIANT VIOLATION 1: CHECK TABLE $_inv_schema.$_inv_tn: $_inv_ct
      --disable_result_log
      --inc $_inv_violations
    }
    --inc $_inv_i
  }
}
DROP TEMPORARY TABLE inv_tabs;

DROP TEMPORARY TABLE inv_cols;
DROP TEMPORARY TABLE inv_allcols;
DROP TEMPORARY TABLE inv_sk;

#
# Invariant 3: MyRocks dictionary vs server data dictionary.
#
# Expected entries: one per (table, partition or subpartition, index), plus
# HIDDEN_PK_ID for tables without a primary key. ROCKSDB_DDL shows table
# and partition names in their file-name encoding (t,x appears as
# t@002cx), so only tables and partitions whose names consist of letters,
# digits and _ are compared; $inv_dict_skipped counts the skipped tables.
# MyRocks names a
# subpartition <partition>#SP#<subpartition>. Names are compared
# case-insensitively in one collation.
#
CREATE TEMPORARY TABLE inv_dd ENGINE=InnoDB AS
SELECT DISTINCT CONVERT(t.TABLE_NAME USING utf8mb4) COLLATE utf8mb4_general_ci AS tn,
       CONVERT(IF(p.SUBPARTITION_NAME IS NULL, p.PARTITION_NAME, CONCAT(p.PARTITION_NAME, '#SP#', p.SUBPARTITION_NAME)) USING utf8mb4) COLLATE utf8mb4_general_ci AS pn,
       CONVERT(s.INDEX_NAME USING utf8mb4) COLLATE utf8mb4_general_ci AS idx
  FROM information_schema.TABLES t
  JOIN information_schema.STATISTICS s
    ON s.TABLE_SCHEMA = t.TABLE_SCHEMA AND s.TABLE_NAME = t.TABLE_NAME
  JOIN information_schema.PARTITIONS p
    ON p.TABLE_SCHEMA = t.TABLE_SCHEMA AND p.TABLE_NAME = t.TABLE_NAME
 WHERE t.TABLE_SCHEMA = @inv_schema AND t.ENGINE = 'ROCKSDB'
   AND t.TABLE_NAME NOT REGEXP '[^[:alnum:]_]'
   AND IFNULL(CONCAT(p.PARTITION_NAME, IFNULL(p.SUBPARTITION_NAME, '')), 'p')
       NOT REGEXP '[^[:alnum:]_]';
INSERT INTO inv_dd
SELECT DISTINCT t.TABLE_NAME,
       IF(p.SUBPARTITION_NAME IS NULL, p.PARTITION_NAME,
          CONCAT(p.PARTITION_NAME, '#SP#', p.SUBPARTITION_NAME)),
       'HIDDEN_PK_ID'
  FROM information_schema.TABLES t
  JOIN information_schema.PARTITIONS p
    ON p.TABLE_SCHEMA = t.TABLE_SCHEMA AND p.TABLE_NAME = t.TABLE_NAME
 WHERE t.TABLE_SCHEMA = @inv_schema AND t.ENGINE = 'ROCKSDB'
   AND t.TABLE_NAME NOT REGEXP '[^[:alnum:]_]'
   AND IFNULL(CONCAT(p.PARTITION_NAME, IFNULL(p.SUBPARTITION_NAME, '')), 'p')
       NOT REGEXP '[^[:alnum:]_]'
   AND NOT EXISTS (SELECT 1 FROM information_schema.STATISTICS s
                    WHERE s.TABLE_SCHEMA = t.TABLE_SCHEMA AND s.TABLE_NAME = t.TABLE_NAME
                      AND s.INDEX_NAME = 'PRIMARY');
CREATE TEMPORARY TABLE inv_rdb ENGINE=InnoDB AS
SELECT CONVERT(TABLE_NAME USING utf8mb4) COLLATE utf8mb4_general_ci AS tn, CONVERT(PARTITION_NAME USING utf8mb4) COLLATE utf8mb4_general_ci AS pn, CONVERT(INDEX_NAME USING utf8mb4) COLLATE utf8mb4_general_ci AS idx,
       COLUMN_FAMILY AS cf, INDEX_NUMBER AS ino
  FROM information_schema.ROCKSDB_DDL
 WHERE TABLE_SCHEMA = @inv_schema
   AND TABLE_NAME NOT REGEXP '[^[:alnum:]_]'
   AND IFNULL(PARTITION_NAME, 'p') NOT REGEXP '[^[:alnum:]_#]';
--let $inv_dict_skipped = `SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = @inv_schema AND ENGINE = 'ROCKSDB' AND TABLE_NAME REGEXP '[^[:alnum:]_]'`
# A temporary table can be opened only once per statement.
CREATE TEMPORARY TABLE inv_v3 ENGINE=InnoDB AS
SELECT 'INVARIANT VIOLATION 3: index missing in MyRocks dictionary' AS v,
       d.tn, d.pn, d.idx
  FROM inv_dd d
 WHERE NOT EXISTS (SELECT 1 FROM inv_rdb r
                    WHERE r.tn = d.tn AND r.pn <=> d.pn AND r.idx = d.idx);
ALTER TABLE inv_v3 MODIFY v VARCHAR(100);
INSERT INTO inv_v3
SELECT 'INVARIANT VIOLATION 3: MyRocks dictionary entry without a table or index',
       r.tn, r.pn, r.idx
  FROM inv_rdb r
 WHERE NOT EXISTS (SELECT 1 FROM inv_dd d
                    WHERE r.tn = d.tn AND r.pn <=> d.pn AND r.idx = d.idx);
INSERT INTO inv_v3
SELECT 'INVARIANT VIOLATION 3: duplicate MyRocks dictionary entry',
       r.tn, r.pn, r.idx
  FROM inv_rdb r
 GROUP BY r.tn, r.pn, r.idx HAVING COUNT(*) > 1;
--let $_inv_n = `SELECT COUNT(*) FROM inv_v3`
if ($_inv_n) {
  --enable_result_log
  SELECT * FROM inv_v3 ORDER BY v, tn, pn, idx;
  --disable_result_log
  --inc $_inv_violations
}
DROP TEMPORARY TABLE inv_v3;
DROP TEMPORARY TABLE inv_dd;
DROP TEMPORARY TABLE inv_rdb;

# Global: an index number is used by one index only, and none is above
# MAX_INDEX_ID.
CREATE TEMPORARY TABLE inv_v3g ENGINE=InnoDB AS
SELECT 'INVARIANT VIOLATION 3: index number used twice' AS v,
       COLUMN_FAMILY AS cf, INDEX_NUMBER AS ino,
       GROUP_CONCAT(CONCAT_WS('.', TABLE_SCHEMA, TABLE_NAME, PARTITION_NAME, INDEX_NAME)
                    ORDER BY TABLE_SCHEMA, TABLE_NAME, PARTITION_NAME, INDEX_NAME) AS users
  FROM information_schema.ROCKSDB_DDL
 GROUP BY COLUMN_FAMILY, INDEX_NUMBER HAVING COUNT(*) > 1
UNION ALL
SELECT 'INVARIANT VIOLATION 3: index number above MAX_INDEX_ID',
       d.COLUMN_FAMILY, d.INDEX_NUMBER,
       CONCAT_WS('.', d.TABLE_SCHEMA, d.TABLE_NAME, d.PARTITION_NAME, d.INDEX_NAME)
  FROM information_schema.ROCKSDB_DDL d
 WHERE d.INDEX_NUMBER > (SELECT CAST(VALUE AS UNSIGNED)
                           FROM information_schema.ROCKSDB_GLOBAL_INFO
                          WHERE TYPE = 'MAX_INDEX_ID');
--let $_inv_n = `SELECT COUNT(*) FROM inv_v3g`
if ($_inv_n) {
  --enable_result_log
  SELECT * FROM inv_v3g ORDER BY v, cf, ino;
  --disable_result_log
  --inc $_inv_violations
}
DROP TEMPORARY TABLE inv_v3g;

#
# Invariant 4: the drop queue drains and leaves no data for dropped indexes.
#
if (!$inv_skip_dropped) {
  --let $_inv_timeout = $inv_drop_timeout
  if (!$_inv_timeout) {
    --let $_inv_timeout = 300
  }
  --let $_inv_drained = 0
  --let $_inv_waited = 0
  while (!$_inv_drained) {
    --let $_inv_drained = `SELECT COUNT(*) = 0 FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'DDL_DROP_INDEX_ONGOING'`
    if (!$_inv_drained) {
      if ($_inv_waited >= $_inv_timeout) {
        --enable_result_log
        --echo INVARIANT VIOLATION 4: drop-index queue did not drain within $_inv_timeout seconds
        SELECT COUNT(*) AS ongoing_drops FROM information_schema.ROCKSDB_GLOBAL_INFO
         WHERE TYPE = 'DDL_DROP_INDEX_ONGOING';
        --disable_result_log
        --inc $_inv_violations
        --let $_inv_drained = 1
        --let $_inv_waited = -1
      }
      if ($_inv_waited >= 0) {
        --sleep 1
        --inc $_inv_waited
      }
    }
  }
  if ($_inv_waited >= 0) {
    # Data reaches SST files only after a flush; the file map covers SSTs only.
    SET GLOBAL rocksdb_force_flush_memtable_now = 1;
    --let $_inv_pass = 1
    while ($_inv_pass <= 2) {
      CREATE TEMPORARY TABLE inv_v4 ENGINE=InnoDB AS
      SELECT 'INVARIANT VIOLATION 4: SST rows for an index not in the dictionary' AS v,
             f.COLUMN_FAMILY AS cf, f.INDEX_NUMBER AS ino, SUM(f.NUM_ROWS) AS num_rows
        FROM information_schema.ROCKSDB_INDEX_FILE_MAP f
       WHERE f.INDEX_NUMBER > 255 AND f.NUM_ROWS > 0
         AND NOT EXISTS (SELECT 1 FROM information_schema.ROCKSDB_DDL d
                          WHERE d.COLUMN_FAMILY = f.COLUMN_FAMILY
                            AND d.INDEX_NUMBER = f.INDEX_NUMBER)
       GROUP BY f.COLUMN_FAMILY, f.INDEX_NUMBER;
      --let $_inv_n = `SELECT COUNT(*) FROM inv_v4`
      if ($_inv_pass == 1) {
        if ($_inv_n) {
          # With rocksdb_enable_delete_range_for_drop_index=ON a drop leaves
          # its rows in SST files under a range tombstone, and the file map
          # counts them until a compaction. Compact each column family that
          # has such rows and check again: rows that survive are a leak.
          --let $_inv_ncf = `SELECT COUNT(DISTINCT cf) FROM inv_v4`
          --let $_inv_c = 1
          while ($_inv_c <= $_inv_ncf) {
            --let $_inv_cf = `SELECT SUBSTRING_INDEX(g.VALUE, ' [', 1) FROM information_schema.ROCKSDB_GLOBAL_INFO g JOIN (SELECT DISTINCT cf, DENSE_RANK() OVER (ORDER BY cf) AS r FROM inv_v4) c ON g.NAME = CAST(c.cf AS CHAR) WHERE g.TYPE = 'CF_FLAGS' AND c.r = $_inv_c`
            --eval SET GLOBAL rocksdb_compact_cf = '$_inv_cf'
            --inc $_inv_c
          }
          DROP TEMPORARY TABLE inv_v4;
        }
        if (!$_inv_n) {
          --let $_inv_pass = 2
        }
      }
      --inc $_inv_pass
    }
    --let $_inv_n = `SELECT COUNT(*) FROM inv_v4`
    if ($_inv_n) {
      --enable_result_log
      SELECT v, cf, ino, num_rows FROM inv_v4 ORDER BY cf, ino;
      --disable_result_log
      --inc $_inv_violations
    }
    DROP TEMPORARY TABLE inv_v4;
  }
}

SET SESSION group_concat_max_len = @inv_saved_gcml;
SET SESSION transaction_isolation = @inv_saved_iso;
--let $inv_violations = $_inv_violations
--enable_result_log
--enable_query_log
if ($inv_die) {
  if ($_inv_violations) {
    --die MyRocks invariants violated, see the INVARIANT VIOLATION lines above
  }
}
