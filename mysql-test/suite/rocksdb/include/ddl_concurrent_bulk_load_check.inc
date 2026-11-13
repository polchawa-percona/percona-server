# Checks after a concurrent bulk load + DDL run:
#  1. no bulk-load temporary files are left in the data directory,
#  2. every dropped index has been removed from the drop-index list, and
#     after a manual compaction no SST file holds keys of an index that no
#     longer exists,
#  3. every table has the same rows through the primary, secondary and
#     unique key (or the partial and full secondary key of bp_0), and
#     CHECK TABLE succeeds.

--disable_query_log
--disable_result_log
let $bl_i = 600;
while ($bl_i)
{
  SET GLOBAL rocksdb_signal_drop_index_thread = 1;
  let $bl_ongoing = query_get_value(SELECT COUNT(*) AS c FROM information_schema.rocksdb_global_info WHERE type = 'DDL_DROP_INDEX_ONGOING', c, 1);
  if ($bl_ongoing == 0)
  {
    let $bl_i = 1;
  }
  if ($bl_ongoing != 0)
  {
    sleep 0.5;
  }
  dec $bl_i;
}
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_compact_cf = 'default';
SET GLOBAL rocksdb_compact_cf = 'bl_cf1';
SET GLOBAL rocksdb_compact_cf = 'rev:bl_cf2';
--enable_result_log
--enable_query_log

--let $bl_datadir = `SELECT @@datadir`
--echo # leftover bulk-load files:
--exec ls $bl_datadir/.rocksdb | grep -c '\.bulk_load\.tmp\.sst$' || true

SELECT COUNT(*) AS drop_index_ongoing FROM information_schema.rocksdb_global_info
 WHERE type = 'DDL_DROP_INDEX_ONGOING';

SELECT COUNT(*) AS orphan_sst_index_entries
  FROM information_schema.rocksdb_index_file_map m
 WHERE m.num_rows > 0
   AND NOT EXISTS (SELECT 1 FROM information_schema.rocksdb_ddl d
                    WHERE d.column_family = m.column_family
                      AND d.index_number = m.index_number)
   AND m.column_family IN (SELECT DISTINCT column_family
                             FROM information_schema.rocksdb_ddl
                            WHERE table_schema = 'test');

--disable_query_log
let $bl_tables = `SELECT GROUP_CONCAT(table_name ORDER BY table_name) FROM information_schema.tables WHERE table_schema = 'test' AND (table_name LIKE 'bl\_%' OR table_name LIKE 'dd\_%')`;
let $bl_n = `SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND (table_name LIKE 'bl\_%' OR table_name LIKE 'dd\_%')`;
--echo # tables: $bl_n
while ($bl_n)
{
  let $bl_t = `SELECT SUBSTRING_INDEX(SUBSTRING_INDEX('$bl_tables', ',', $bl_n), ',', -1)`;
  let $bl_pk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(id),0), ':', COALESCE(SUM(k),0)) FROM $bl_t FORCE INDEX(PRIMARY)`;
  let $bl_sk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(id),0), ':', COALESCE(SUM(k),0)) FROM $bl_t FORCE INDEX(sk)`;
  let $bl_uk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(id),0), ':', COALESCE(SUM(k),0)) FROM $bl_t FORCE INDEX(uk)`;
  if ($bl_pk != $bl_sk)
  {
    --echo $bl_t: PRIMARY $bl_pk vs sk $bl_sk
  }
  if ($bl_pk != $bl_uk)
  {
    --echo $bl_t: PRIMARY $bl_pk vs uk $bl_uk
  }
  let $bl_chk = query_get_value(CHECK TABLE $bl_t, Msg_text, 1);
  if ($bl_chk != OK)
  {
    --echo $bl_t: CHECK TABLE $bl_chk
  }
  dec $bl_n;
}
let $bl_pk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(g),0), ':', COALESCE(SUM(k),0)) FROM bp_0 FORCE INDEX(PRIMARY)`;
let $bl_sk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(g),0), ':', COALESCE(SUM(k),0)) FROM bp_0 FORCE INDEX(pk)`;
let $bl_uk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(g),0), ':', COALESCE(SUM(k),0)) FROM bp_0 FORCE INDEX(fk)`;
if ($bl_pk != $bl_sk)
{
  --echo bp_0: PRIMARY $bl_pk vs partial index $bl_sk
}
if ($bl_pk != $bl_uk)
{
  --echo bp_0: PRIMARY $bl_pk vs full index $bl_uk
}
let $bl_chk = query_get_value(CHECK TABLE bp_0, Msg_text, 1);
if ($bl_chk != OK)
{
  --echo bp_0: CHECK TABLE $bl_chk
}
--echo # index consistency checked
--enable_query_log
