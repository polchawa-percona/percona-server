# ==== Purpose ====
#
# List the internal DDL tables of a schema that exist in the MyRocks
# dictionary or in the server data dictionary: #sql-* and #sql2-* (copies
# made by ALTER TABLE) and #truncate_tmp#* (the old table of TRUNCATE
# TABLE). After a statement has finished, with success or with an error,
# none of them may be left (PS-12198 left #sql2-* and #truncate_tmp#*).
#
# The server data dictionary hides these tables from information_schema,
# so the DD side reads mysql.tables, which needs a debug build
# (skip_dd_table_access_check). On other builds only the MyRocks dictionary
# is checked and $inv_orphans_dd is 0.
#
# ==== Usage ====
#
# [--let $inv_schema = db1]          # default: DATABASE()
# --source suite/rocksdb/include/myrocks_inv_orphans.inc
# --echo $inv_orphans                # "none" or a sorted comma list of
#                                    # rdb:<name> and dd:<name> entries
# $inv_orphans_dd is 1 when the server data dictionary was checked.
#
# Names are listed as stored: ROCKSDB_DDL shows names in the file-name
# encoding, where # may appear as @0023.
#
# $inv_orphans_abort_ok lists ("none" if empty) the entries that are
# accepted after a server abort (never after a statement that returned):
# a #sql-* table (not #sql2-*, not #truncate_tmp#*) that exists only in the
# server data dictionary. For a storage engine without atomic DDL the server
# stores the new table version of every ALTER TABLE under its temporary
# name in the data dictionary and commits it before the engine works on it
# (sql/sql_table.cc, create_table_impl() in mysql_alter_table() with
# no_ha_table unless HTON_SUPPORTS_ATOMIC_DDL), so an abort leaves that
# hidden entry. It holds no data, does not hinder later DDL or restarts,
# and DROP DATABASE removes it.

--disable_query_log
--disable_result_log
--let $_invo_schema = $inv_schema
if (!$_invo_schema) {
  --let $_invo_schema = `SELECT DATABASE()`
}
--eval SET @invo_schema = '$_invo_schema'

SET @invo_rdb = NULL;
SELECT GROUP_CONCAT(DISTINCT CONCAT('rdb:', TABLE_NAME, IF(PARTITION_NAME IS NULL, '', CONCAT('#P#', PARTITION_NAME)))
                    ORDER BY TABLE_NAME, PARTITION_NAME)
  INTO @invo_rdb
  FROM information_schema.ROCKSDB_DDL
 WHERE TABLE_SCHEMA = @invo_schema
   AND TABLE_NAME REGEXP '^(#|@0023)(sql|truncate_tmp)';

--let $inv_orphans_dd = `SELECT COUNT(*) FROM performance_schema.global_variables WHERE VARIABLE_NAME = 'debug'`
SET @invo_dd = NULL;
if ($inv_orphans_dd) {
  SET @invo_saved_debug = @@SESSION.debug;
  SET SESSION debug = '+d,skip_dd_table_access_check';
  SELECT GROUP_CONCAT(CONCAT('dd:', t.name) ORDER BY t.name)
    INTO @invo_dd
    FROM mysql.tables t JOIN mysql.schemata s ON s.id = t.schema_id
   WHERE s.name = @invo_schema
     AND (t.name LIKE '#sql%' OR t.name LIKE '#truncate\_tmp#%');
  SET SESSION debug = @invo_saved_debug;
}
SET @invo_all = CONCAT_WS(',', @invo_rdb, @invo_dd);
SET @invo_abort_ok = NULL;
if ($inv_orphans_dd) {
  SELECT GROUP_CONCAT(j.n ORDER BY j.n) INTO @invo_abort_ok
    FROM JSON_TABLE(CONCAT('["', REPLACE(CONVERT(IFNULL(@invo_dd, '') USING utf8mb4), ',', '","'), '"]'),
                    '$[*]' COLUMNS (n VARCHAR(512) PATH '$')) j
   WHERE j.n LIKE 'dd:#sql-%'
     AND NOT FIND_IN_SET(CONCAT('rdb:', SUBSTRING(j.n, 4)), CONVERT(IFNULL(@invo_rdb, '') USING utf8mb4));
}
--let $inv_orphans_abort_ok = `SELECT IFNULL(REPLACE(@invo_abort_ok, ' ', '_'), 'none')`
--let $inv_orphans = `SELECT IF(@invo_all = '', 'none', REPLACE(@invo_all, ' ', '_'))`
--enable_result_log
--enable_query_log
