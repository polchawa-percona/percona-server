# ==== Purpose ====
#
# Fingerprint the full rows of every MyRocks table of a schema, read through
# the primary key (or the hidden primary key): per table the row count and
# the sum over all rows of a CRC32 over every column, including BLOB, TEXT,
# JSON and other non-key columns. Unlike checksums over the indexed columns,
# this catches a lost or wrong value in a column that no index covers.
#
# Each column is hashed separately (CRC32 of its bytes, 'N' for NULL) before
# the row hash, so that a large BLOB cannot push the row expression over
# max_allowed_packet (which would turn it into NULL and hide the row).
#
# The fingerprint does not depend on index ids or on the order of rows. The
# caller compares it with a fingerprint taken at another time, for example
# before and after a statement, or before and after a restart.
#
# ==== Usage ====
#
# [--let $inv_schema = db1]          # default: DATABASE()
# [--let $inv_fullrow_skip_cols = t1.ts,t2.c]  # columns left out, for
#                                    # values that depend on the time a
#                                    # fixture ran (UNIX_TIMESTAMP())
# --source suite/rocksdb/include/myrocks_inv_fullrow.inc
# --echo $inv_fullrow                # "t1=<rows>/<sum>;t2=..." or "no_tables"
#
# The value contains no spaces. A table that cannot be read appears as
# "<table>=unreadable(<errno>)"; the other tables keep their values.

--disable_query_log
--disable_result_log
--let $_invf_schema = $inv_schema
if (!$_invf_schema) {
  --let $_invf_schema = `SELECT DATABASE()`
}
SET @invf_saved_gcml = @@SESSION.group_concat_max_len;
SET SESSION group_concat_max_len = 16 * 1024 * 1024;
--eval SET @invf_schema = '$_invf_schema'
--eval SET @invf_skip = '$inv_fullrow_skip_cols'
SET @invf_qs = REPLACE(@invf_schema, '`', '``');

# One query per table, so that a table that cannot be opened does not hide
# the others; USE INDEX () makes MyRocks read the primary key (hidden or
# not).
CREATE TEMPORARY TABLE invf_q ENGINE=InnoDB AS
SELECT ROW_NUMBER() OVER (ORDER BY x.tn) AS n,
       REPLACE(x.tn, ' ', '_') AS tn,
       CONCAT('SELECT CONCAT(', QUOTE(REPLACE(x.tn, ' ', '_')), ', ''='', COUNT(*), ''/'', ',
              'IFNULL(SUM(CRC32(CONCAT_WS(''#'', ', x.expr, '))), 0)) INTO @invf_one FROM `',
              @invf_qs, '`.`', REPLACE(x.tn, '`', '``'), '` USE INDEX ()') AS q
  FROM (SELECT c.TABLE_NAME AS tn,
               GROUP_CONCAT(CONCAT('IFNULL(CRC32(CAST(`', REPLACE(c.COLUMN_NAME, '`', '``'),
                                   '` AS BINARY)), ''N'')')
                            ORDER BY c.ORDINAL_POSITION) AS expr
          FROM information_schema.COLUMNS c
          JOIN information_schema.TABLES t
            ON t.TABLE_SCHEMA = c.TABLE_SCHEMA AND t.TABLE_NAME = c.TABLE_NAME
         WHERE c.TABLE_SCHEMA = @invf_schema AND t.ENGINE = 'ROCKSDB'
           AND t.TABLE_TYPE = 'BASE TABLE'
           AND NOT FIND_IN_SET(CONCAT(c.TABLE_NAME, '.', c.COLUMN_NAME), @invf_skip)
         GROUP BY c.TABLE_NAME) x;

--let $inv_fullrow = no_tables
--let $_invf_n = `SELECT COUNT(*) FROM invf_q`
if ($_invf_n) {
  SET @invf_res = NULL;
  --let $_invf_i = 1
  while ($_invf_i <= $_invf_n) {
    --eval SELECT q, tn INTO @invf_q, @invf_tn FROM invf_q WHERE n = $_invf_i
    SET @invf_one = NULL;
    --disable_abort_on_error
    # The table is opened by PREPARE already; keep the first error.
    PREPARE invf_stmt FROM @invf_q;
    --let $_invf_err = $mysql_errno
    if (!$_invf_err) {
      EXECUTE invf_stmt;
      --let $_invf_err = $mysql_errno
      DEALLOCATE PREPARE invf_stmt;
    }
    --enable_abort_on_error
    if ($_invf_err) {
      --eval SET @invf_one = CONCAT(@invf_tn, '=unreadable($_invf_err)')
    }
    SET @invf_res = CONCAT_WS(';', @invf_res, @invf_one);
    --inc $_invf_i
  }
  --let $inv_fullrow = `SELECT REPLACE(@invf_res, ' ', '_')`
}
DROP TEMPORARY TABLE invf_q;
SET SESSION group_concat_max_len = @invf_saved_gcml;
--enable_result_log
--enable_query_log
