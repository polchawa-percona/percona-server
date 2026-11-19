#
# Creates test.$errinj_bg_table with a primary key and, unless
# $errinj_bg_pk_only is set, a secondary key, fills it with 2000 rows and
# flushes it to an SST file.
#
--disable_query_log
eval CREATE TABLE $errinj_bg_table (pk INT PRIMARY KEY, a INT, b VARCHAR(32)
  $errinj_bg_keys) ENGINE=ROCKSDB;
SET SESSION cte_max_recursion_depth = 10000;
eval INSERT INTO $errinj_bg_table
  WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM s WHERE i < 2000)
  SELECT i, i MOD 97, CONCAT('row', i) FROM s;
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
--enable_query_log
