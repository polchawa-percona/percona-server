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
#   8. No internal DDL table (#sql*, #sql2*, #truncate_tmp#*) is left in
#      the MyRocks dictionary or (debug builds) in the server data
#      dictionary (myrocks_inv_orphans.inc). A #sql-* that exists only in
#      the server data dictionary is what an abort during ALTER TABLE
#      leaves; it is reported too, unless $inv_after_abort = 1.
#   9. With $inv_fullrow_expect set: the full rows of every MyRocks table,
#      read through the primary key (every column, myrocks_inv_fullrow.inc),
#      are the same as when $inv_fullrow_expect was taken, for example
#      before a restart. $inv_fullrow always returns the current value.
#  10. Every MyRocks table can be opened; an unreadable table is reported
#      (with its error code) and the other tables are still checked.
#  11. (with invariant 4 hits) The persisted MyRocks dictionary has no
#      table entry that the in-memory map (ROCKSDB_DDL) lacks: SST rows of
#      an index of such an entry are reported as invariant 11, not 4. The
#      persisted dictionary is not readable from SQL; it is read with ldb
#      from a checkpoint (see invariant 4 below; $inv_pdict_checked = 0
#      when that was not possible: a NOTE line then names the cause and
#      the rows stay invariant 4 with persisted_table "unverified").
#      Only evaluated when invariant 4 has hits: a persisted-only entry
#      without SST rows is not seen, and invariant 8 (names) reads
#      ROCKSDB_DDL too, so it stays invisible until a restart reloads it.
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
# [--let $inv_skip_orphans = 1]      # skip invariant 8
# [--let $inv_orphans_known = <list>] # comma list (as returned in
#                                    # $inv_orphans) that invariant 8 ignores
# [--let $inv_after_abort = 1]       # the caller knows that the server
#                                    # aborted inside a DDL statement on this
#                                    # schema: invariant 8 accepts the entries
#                                    # of $inv_orphans_abort_ok (a #sql-*
#                                    # only in the server data dictionary,
#                                    # see myrocks_inv_orphans.inc). Leftovers
#                                    # of earlier tests can be listed in
#                                    # $inv_orphans_known instead.
# [--let $inv_skip_partial = 1]      # skip partial indexes in invariant 1
# [--let $inv_fullrow_expect = <v>]  # $inv_fullrow of an earlier run
# [--let $inv_mask_index_numbers = 1] # print index numbers of invariant
#                                    # 4 and 11 rows as <ino> (for a
#                                    # recorded result)
# --source suite/rocksdb/include/myrocks_invariants.inc
#
# CHECK TABLE is skipped for tables whose name contains a comma (mysqltest
# cannot run it for them); $inv_check_skipped returns how many were skipped.
# Invariant 3 skips tables whose name needs file-name encoding;
# $inv_dict_skipped returns how many were skipped.
#
# Invariant 1 also reads invisible indexes (use_invisible_indexes=on) and
# partial indexes (partial_group comment). Reading a partial index
# materializes every group of at least partial_group_threshold rows that is
# not materialized yet (rocksdb_partial_index_groups_materialized grows), as
# any query through that index would; an already materialized group is read
# from the index, so stale or missing entries in it show up. Groups below the
# threshold are read from the primary key, so the comparison is trivially
# equal for them. A prefix index cannot return rows in column order, so its
# side of the comparison may fall back to a table scan and only CHECK TABLE
# checks it. Functional key parts are compared by their expression value:
# the secondary index side orders by the expression (an index scan) and
# computes the value from the row, so it checks the row count and the
# rows' membership in the index.

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
# FORCE INDEX of an invisible index needs this.
SET @inv_saved_osw = @@SESSION.optimizer_switch;
SET SESSION optimizer_switch = 'use_invisible_indexes=on';
--eval SET @inv_schema = '$_inv_schema'
--let $_inv_skip_partial = 0
if ($inv_skip_partial) {
  --let $_inv_skip_partial = 1
}
--eval SET @inv_skip_partial = $_inv_skip_partial

# Open every MyRocks table of the schema first: until a table is opened
# after a restart, ROCKSDB_DDL shows empty index names for it (PS-11882),
# which invariant 3 would report as a mismatch.
#
# Invariant 10: every MyRocks table of the server data dictionary can be
# opened. A table that cannot be opened (e.g. ER 1815 "not present in
# RocksDB-SE data dictionary" after a failed DROP or a crash inside a
# rename) is reported once, with the error code, and left out of
# invariant 1 and CHECK TABLE; the other tables are still checked.
# Invariant 3 and 9 still see it (its dictionary entries, and
# "<table>=unreadable(<errno>)" in the full-row fingerprint).
# $inv_unreadable returns the comma list of such tables ("" if none).
CREATE TEMPORARY TABLE inv_tabs ENGINE=InnoDB AS
SELECT TABLE_NAME AS tn, ROW_NUMBER() OVER (ORDER BY TABLE_NAME) AS n,
       0 AS err
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = @inv_schema AND ENGINE = 'ROCKSDB';
--let $_inv_ntabs = `SELECT COUNT(*) FROM inv_tabs`
--let $_inv_i = 1
while ($_inv_i <= $_inv_ntabs) {
  --let $_inv_tn = `SELECT REPLACE(tn, CHAR(96), REPEAT(CHAR(96), 2)) FROM inv_tabs WHERE n = $_inv_i`
  --disable_abort_on_error
  --eval SELECT 1 FROM `$_inv_schema`.`$_inv_tn` LIMIT 0
  --let $_inv_err = $mysql_errno
  --enable_abort_on_error
  if ($_inv_err) {
    --eval UPDATE inv_tabs SET err = $_inv_err WHERE n = $_inv_i
  }
  --inc $_inv_i
}
--let $inv_unreadable = `SELECT IFNULL(GROUP_CONCAT(tn ORDER BY tn), '') FROM inv_tabs WHERE err <> 0`
--let $_inv_n = `SELECT COUNT(*) FROM inv_tabs WHERE err <> 0`
if ($_inv_n) {
  --enable_result_log
  SELECT 'INVARIANT VIOLATION 10: table unreadable' AS v, tn, err
    FROM inv_tabs WHERE err <> 0 ORDER BY tn;
  --disable_result_log
  --inc $_inv_violations
}
CREATE TEMPORARY TABLE inv_bad ENGINE=InnoDB AS
SELECT CONVERT(tn USING utf8mb4) COLLATE utf8mb4_general_ci AS tn FROM inv_tabs WHERE err <> 0;

#
# Invariant 1: secondary index vs primary key, per table and index.
#
# Each secondary index is read in index order with FORCE INDEX, which makes
# the optimizer scan the index; the primary side reads the table with no
# index allowed, which is a primary-key scan in MyRocks. Both sides
# checksum the index columns plus the primary key columns.
#
# A functional key part has no COLUMN_NAME; it is used by its EXPRESSION
# (information_schema shows quotes in it escaped as \', which is undone).
CREATE TEMPORARY TABLE inv_cols ENGINE=InnoDB AS
SELECT s.TABLE_NAME AS tn, s.INDEX_NAME AS idx,
       GROUP_CONCAT(DISTINCT IF(s.COLUMN_NAME IS NULL, CONCAT('(', REPLACE(s.EXPRESSION, CONCAT(CHAR(92), CHAR(39)), CHAR(39)), ')'),
                                CONCAT('`', REPLACE(s.COLUMN_NAME, '`', '``'), '`'))
                    ORDER BY s.SEQ_IN_INDEX) AS cols
  FROM information_schema.STATISTICS s
  JOIN information_schema.TABLES t
    ON t.TABLE_SCHEMA = s.TABLE_SCHEMA AND t.TABLE_NAME = s.TABLE_NAME
 WHERE s.TABLE_SCHEMA = @inv_schema AND t.ENGINE = 'ROCKSDB'
   AND (s.COLUMN_NAME IS NOT NULL OR s.EXPRESSION IS NOT NULL)
 GROUP BY s.TABLE_NAME, s.INDEX_NAME;

CREATE TEMPORARY TABLE inv_sk ENGINE=InnoDB AS
SELECT DISTINCT s.TABLE_NAME AS tn, s.INDEX_NAME AS idx
  FROM information_schema.STATISTICS s
  JOIN information_schema.TABLES t
    ON t.TABLE_SCHEMA = s.TABLE_SCHEMA AND t.TABLE_NAME = s.TABLE_NAME
 WHERE s.TABLE_SCHEMA = @inv_schema AND t.ENGINE = 'ROCKSDB'
   AND s.INDEX_NAME <> 'PRIMARY'
   AND (s.INDEX_COMMENT NOT LIKE '%partial_group%' OR @inv_skip_partial = 0)
   AND NOT EXISTS (SELECT 1 FROM inv_bad b
                    WHERE b.tn = CONVERT(s.TABLE_NAME USING utf8mb4) COLLATE utf8mb4_general_ci);

# Columns (and functional key part expressions) of the index plus the
# primary key columns, each once, as a checksum expression. Identifiers are quoted with
# backticks (doubled inside), so any column name works.
CREATE TEMPORARY TABLE inv_allcols ENGINE=InnoDB AS
SELECT k.tn, k.idx,
       COALESCE(GROUP_CONCAT(DISTINCT IF(s.COLUMN_NAME IS NULL,
                                         CONCAT('IFNULL(HEX(CAST((', REPLACE(s.EXPRESSION, CONCAT(CHAR(92), CHAR(39)), CHAR(39)), ') AS BINARY)),''N'')'),
                                         CONCAT('IFNULL(HEX(CAST(`',
                                                REPLACE(s.COLUMN_NAME, '`', '``'),
                                                '` AS BINARY)),''N'')'))
                             ORDER BY IFNULL(s.COLUMN_NAME, s.EXPRESSION)), '1') AS expr
  FROM inv_sk k
  LEFT JOIN information_schema.STATISTICS s
    ON s.TABLE_SCHEMA = @inv_schema AND s.TABLE_NAME = k.tn
   AND s.INDEX_NAME IN (k.idx, 'PRIMARY')
   AND (s.COLUMN_NAME IS NOT NULL OR s.EXPRESSION IS NOT NULL)
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
    '(SELECT SUM(d.h) FROM (SELECT CRC32(CONCAT_WS(''#'',', ac.expr, ')) AS h FROM `',
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
    # An unreadable table is already reported (invariant 10).
    --let $_inv_bad = `SELECT err <> 0 FROM inv_tabs WHERE n = $_inv_i`
    --let $_inv_ct = OK
    if ($_inv_comma) {
      --inc $inv_check_skipped
    }
    if ($_inv_bad) {
      --let $_inv_comma = 1
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
DROP TEMPORARY TABLE inv_bad;

DROP TEMPORARY TABLE inv_cols;
DROP TEMPORARY TABLE inv_allcols;
DROP TEMPORARY TABLE inv_sk;

#
# Invariant 3: MyRocks dictionary vs server data dictionary.
#
# Expected entries: one per (table, partition or subpartition, index), plus
# HIDDEN_PK_ID for tables without a primary key. A table without a PRIMARY
# KEY but with a UNIQUE key whose parts are all NOT NULL columns, not
# prefixes and not virtual generated columns (functional key parts are
# virtual generated columns) has no hidden primary key: the server uses the
# first such key as the primary key (is_suitable_for_primary_key() in
# sql/dd_table_share.cc). SUB_PART is NULL exactly when the key part
# covers the whole column; a BLOB/TEXT key part always has a prefix and
# qualifies when the prefix (SUB_PART characters times the character set's
# MAXLEN bytes) equals the column's length in bytes. ROCKSDB_DDL shows table
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
# Tables with a UNIQUE key the server promotes to the primary key. A
# grouped query: nested EXISTS subqueries over these views in an
# INSERT ... SELECT hit an optimizer assertion in
# Join_node::get_const_ordered_table_node() on debug builds.
CREATE TEMPORARY TABLE inv_cand ENGINE=InnoDB AS
SELECT DISTINCT CONVERT(k.TABLE_NAME USING utf8mb4) COLLATE utf8mb4_general_ci AS tn
  FROM information_schema.STATISTICS k
  LEFT JOIN information_schema.COLUMNS c
    ON c.TABLE_SCHEMA = k.TABLE_SCHEMA AND c.TABLE_NAME = k.TABLE_NAME
   AND c.COLUMN_NAME = k.COLUMN_NAME
  LEFT JOIN information_schema.CHARACTER_SETS cs
    ON cs.CHARACTER_SET_NAME = c.CHARACTER_SET_NAME
 WHERE k.TABLE_SCHEMA = @inv_schema AND k.NON_UNIQUE = 0
 GROUP BY k.TABLE_NAME, k.INDEX_NAME
HAVING SUM(k.COLUMN_NAME IS NULL OR k.NULLABLE = 'YES'
           OR IFNULL(c.EXTRA LIKE '%VIRTUAL GENERATED%', 0)
           OR (k.SUB_PART IS NOT NULL
               AND NOT (c.DATA_TYPE IN ('tinyblob', 'blob', 'mediumblob', 'longblob',
                                        'tinytext', 'text', 'mediumtext', 'longtext')
                        AND k.SUB_PART * IFNULL(cs.MAXLEN, 1) <=> c.CHARACTER_OCTET_LENGTH))) = 0;
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
                      AND s.INDEX_NAME = 'PRIMARY')
   AND NOT EXISTS (SELECT 1 FROM inv_cand c
                    WHERE c.tn = CONVERT(t.TABLE_NAME USING utf8mb4) COLLATE utf8mb4_general_ci);
DROP TEMPORARY TABLE inv_cand;
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
    # Wake the drop-index thread, which otherwise sleeps up to a minute
    # between rounds (a pending drop, e.g. after a restart, then makes
    # every invariants run wait for it).
    SET GLOBAL rocksdb_signal_drop_index_thread = 1;
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
    --let $inv_pdict_checked = 0
    if ($_inv_n) {
      # ROCKSDB_DDL (and so the check above) shows the in-memory table map
      # only. A table can be missing there while its entry is still in the
      # persisted MyRocks dictionary (a DROP or TRUNCATE whose dictionary
      # commit failed, PS-12220, PS-12217): its index data is not orphaned
      # yet (a restart reloads the entry). The persisted dictionary is not
      # readable from SQL, so it is read from a checkpoint with the ldb tool
      # of the build (next to $MYSQL_SST_DUMP), only when there are hits.
      # Rows whose index number belongs to a persisted table entry are
      # reported as invariant 11 (dictionary/map mismatch); the others stay
      # invariant 4 (index data without any dictionary entry). Without ldb,
      # or if the checkpoint or ldb fails, a NOTE line names the cause, the
      # rows stay invariant 4 with persisted_table "unverified" (they may be
      # invariant 11), and $inv_pdict_checked is 0.
      --let INV_PDICT_CKPT = $MYSQLTEST_VARDIR/tmp/inv_pdict_ckpt
      --let INV_PDICT_OUT = $MYSQLTEST_VARDIR/tmp/inv_pdict.txt
      --remove_files_wildcard $MYSQLTEST_VARDIR/tmp inv_pdict.txt
      # A checkpoint left by an aborted earlier run would be read instead.
      --perl
        use File::Path qw(rmtree);
        rmtree($ENV{INV_PDICT_CKPT}) if -d $ENV{INV_PDICT_CKPT};
      EOF
      --disable_abort_on_error
      --eval SET GLOBAL rocksdb_create_checkpoint = '$MYSQLTEST_VARDIR/tmp/inv_pdict_ckpt'
      --let INV_PDICT_SET_ERR = $mysql_errno
      --enable_abort_on_error
      --perl
        use strict;
        use File::Path qw(rmtree);
        my $ck = $ENV{INV_PDICT_CKPT};
        my $out = $ENV{INV_PDICT_OUT};
        (my $dir = ($ENV{MYSQL_SST_DUMP} // '')) =~ s{/[^/]*$}{};
        my $ldb = "$dir/ldb";
        open(my $o, '>', $out) or die "cannot write $out: $!";
        # The last line is the status: "ok" or the reason the persisted
        # dictionary was not read.
        my $st;
        if ($ENV{INV_PDICT_SET_ERR}) {
          $st = "checkpoint error $ENV{INV_PDICT_SET_ERR}";
        } elsif (!-d $ck) {
          $st = 'checkpoint directory missing';
        } elsif (!$ENV{MYSQL_SST_DUMP}) {
          $st = 'ldb not found (MYSQL_SST_DUMP unset)';
        } elsif (!-x $ldb) {
          $st = "ldb not found at $ldb";
        }
        if (!defined $st) {
          # Key: 0x00000001 (DDL_ENTRY_INDEX_START_NUMBER) + "db.table";
          # value: 2-byte version, then (cf_id, index_id) 4 bytes each.
          if (open(my $p, '-|', $ldb, "--db=$ck", '--column_family=__system__',
                   'scan', '--hex')) {
            while (my $l = <$p>) {
              next unless $l =~ /^0x00000001([0-9A-F]*) ==> 0x([0-9A-F]+)/;
              my ($n, $v) = (pack('H*', $1), $2);
              $n =~ s/[\t\n\\]/_/g;
              for (my $i = 4; $i + 16 <= length($v); $i += 16) {
                printf $o "%d\t%d\t%s\n", hex(substr($v, $i, 8)),
                  hex(substr($v, $i + 8, 8)), $n;
              }
            }
            $st = close($p) ? 'ok' : 'ldb failed rc=' . ($? >> 8);
          } else {
            $st = "ldb failed to start: $!";
          }
        }
        $st =~ s/[\t\n\\]/_/g;
        print $o "0\t0\t#status $st\n";
        close($o);
        rmtree($ck) if -d $ck;
      EOF
      CREATE TEMPORARY TABLE inv_pdict (cf BIGINT, ino BIGINT, tn VARCHAR(600)) ENGINE=InnoDB;
      --eval LOAD DATA INFILE '$MYSQLTEST_VARDIR/tmp/inv_pdict.txt' INTO TABLE inv_pdict
      --remove_file $MYSQLTEST_VARDIR/tmp/inv_pdict.txt
      --let $_inv_pdict_status = `SELECT IFNULL(MAX(SUBSTRING(tn, 9)), 'no status line') FROM inv_pdict WHERE tn LIKE '#status %'`
      --let $inv_pdict_checked = 0
      if ($_inv_pdict_status == ok) {
        --let $inv_pdict_checked = 1
      }
      ALTER TABLE inv_v4 MODIFY v VARCHAR(200), ADD COLUMN persisted_table VARCHAR(600);
      if ($inv_pdict_checked) {
        UPDATE inv_v4 v4 JOIN inv_pdict p ON p.cf = v4.cf AND p.ino = v4.ino AND p.tn NOT LIKE '#status %'
           SET v4.v = 'INVARIANT VIOLATION 11: index data of a table in the persisted MyRocks dictionary only (dictionary/map mismatch)',
               v4.persisted_table = p.tn;
      }
      if (!$inv_pdict_checked) {
        UPDATE inv_v4 SET persisted_table = 'unverified';
      }
      DROP TEMPORARY TABLE inv_pdict;
      --enable_result_log
      if (!$inv_pdict_checked) {
        --echo NOTE invariant 4: the persisted MyRocks dictionary was not read ($_inv_pdict_status); rows marked unverified may be invariant 11
      }
      if ($inv_mask_index_numbers) {
        --replace_column 3 <ino>
      }
      SELECT v, cf, ino, num_rows, persisted_table FROM inv_v4 ORDER BY v, cf, ino;
      --disable_result_log
      --inc $_inv_violations
    }
    DROP TEMPORARY TABLE inv_v4;
    --let INV_PDICT_CKPT =
    --let INV_PDICT_OUT =
    --let INV_PDICT_SET_ERR =
  }
}

#
# Invariant 8: no internal DDL table is left.
#
if (!$inv_skip_orphans) {
  --let $_inv_save_schema = $inv_schema
  --let $inv_schema = $_inv_schema
  --source suite/rocksdb/include/myrocks_inv_orphans.inc
  --let $inv_schema = $_inv_save_schema
  --disable_query_log
  --disable_result_log
  if ($inv_orphans != none) {
    --eval SET @inv_orph = '$inv_orphans', @inv_orph_known = '$inv_orphans_known'
    if ($inv_after_abort) {
      --eval SET @inv_orph_known = CONCAT_WS(',', @inv_orph_known, '$inv_orphans_abort_ok')
    }
    --let $_inv_orph_new = `SELECT IFNULL(GROUP_CONCAT(j.n ORDER BY j.n), '') FROM JSON_TABLE(CONCAT('["', REPLACE(@inv_orph, ',', '","'), '"]'), '$[*]' COLUMNS (n VARCHAR(512) PATH '$')) j WHERE NOT FIND_IN_SET(j.n, @inv_orph_known)`
    # $inv_orphans_left returns the names; the line printed masks the
    # process and thread ids in them, so that the output is stable.
    --let $inv_orphans_left = $_inv_orph_new
    if ($_inv_orph_new) {
      --let $_inv_orph_new = `SELECT REGEXP_REPLACE(REGEXP_REPLACE('$_inv_orph_new', '#sql2-[0-9a-f]+-[0-9a-f]+', '#sql2-<id>'), '#sql-[0-9a-f]+_[0-9a-f]+', '#sql-<id>')`
      --enable_result_log
      --echo INVARIANT VIOLATION 8: internal DDL table left in $_inv_schema: $_inv_orph_new
      --disable_result_log
      --inc $_inv_violations
    }
  }
}

#
# Invariant 9: full rows unchanged since $inv_fullrow_expect was taken.
#
--let $_inv_save_schema = $inv_schema
--let $inv_schema = $_inv_schema
--source suite/rocksdb/include/myrocks_inv_fullrow.inc
--let $inv_schema = $_inv_save_schema
--disable_query_log
--disable_result_log
if ($inv_fullrow_expect) {
  if ($inv_fullrow != $inv_fullrow_expect) {
    --enable_result_log
    --echo INVARIANT VIOLATION 9: full rows differ from the expected fingerprint
    --echo   expected: $inv_fullrow_expect
    --echo   now:      $inv_fullrow
    --disable_result_log
    --inc $_inv_violations
  }
}

SET SESSION group_concat_max_len = @inv_saved_gcml;
SET SESSION transaction_isolation = @inv_saved_iso;
SET SESSION optimizer_switch = @inv_saved_osw;
--let $inv_violations = $_inv_violations
--enable_result_log
--enable_query_log
if ($inv_die) {
  if ($_inv_violations) {
    --die MyRocks invariants violated, see the INVARIANT VIOLATION lines above
  }
}
