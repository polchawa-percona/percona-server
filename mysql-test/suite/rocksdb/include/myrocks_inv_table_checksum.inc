# ==== Purpose ====
#
# Compute a content checksum of one table: row count and the sum of a CRC32
# over all columns of each row. Used by the invariant helpers.
#
# ==== Usage ====
#
# --let $inv_table = t1
# [--let $inv_schema = db1]   # default: DATABASE()
# --source suite/rocksdb/include/myrocks_inv_table_checksum.inc
# --echo $inv_checksum        # "<rows>:<sum>"

--disable_query_log
--let $_invc_schema = $inv_schema
if (!$_invc_schema) {
  --let $_invc_schema = `SELECT DATABASE()`
}
SET @invc_expr = NULL;
--eval SELECT GROUP_CONCAT(CONCAT('IFNULL(HEX(CAST(', CHAR(96), REPLACE(COLUMN_NAME, CHAR(96), REPEAT(CHAR(96), 2)), CHAR(96), ' AS BINARY)),''N'')') ORDER BY ORDINAL_POSITION) INTO @invc_expr FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '$_invc_schema' AND TABLE_NAME = '$inv_table'
--let $_invc_found = `SELECT @invc_expr IS NOT NULL`
if (!$_invc_found) {
  --die myrocks_inv_table_checksum.inc: table $_invc_schema.$inv_table not found
}
--eval SET @invc_q = CONCAT('SELECT CONCAT(COUNT(*), '':'', IFNULL(SUM(CRC32(CONCAT_WS(''#'', ', @invc_expr, '))), 0)) AS c FROM ', CHAR(96), '$_invc_schema', CHAR(96), '.', CHAR(96), '$inv_table', CHAR(96))
PREPARE invc_stmt FROM @invc_q;
--let $inv_checksum = query_get_value(EXECUTE invc_stmt, c, 1)
DEALLOCATE PREPARE invc_stmt;
--enable_query_log
