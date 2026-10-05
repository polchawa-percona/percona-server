# Checks after a concurrent DDL run:
#  1. every dropped index has been removed from the drop-index list,
#  2. after a manual compaction of the test column families no SST file
#     holds keys of an index that no longer exists,
#  3. the survivor tables have all their rows through both indexes.
# Requires $cc_rows (rows per survivor).

--disable_query_log
--disable_result_log
let $cc_i = 600;
while ($cc_i)
{
  SET GLOBAL rocksdb_signal_drop_index_thread = 1;
  let $cc_ongoing = query_get_value(SELECT COUNT(*) AS c FROM information_schema.rocksdb_global_info WHERE type = 'DDL_DROP_INDEX_ONGOING', c, 1);
  if ($cc_ongoing == 0)
  {
    let $cc_i = 1;
  }
  if ($cc_ongoing != 0)
  {
    sleep 0.5;
  }
  dec $cc_i;
}
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_compact_cf = 'default';
SET GLOBAL rocksdb_compact_cf = 'cc_cf1';
SET GLOBAL rocksdb_compact_cf = 'rev:cc_cf2';
--enable_result_log
--enable_query_log

SELECT COUNT(*) AS drop_index_ongoing FROM information_schema.rocksdb_global_info
 WHERE type = 'DDL_DROP_INDEX_ONGOING';

# Keys of indexes that no longer exist, in the test column families.
SELECT COUNT(*) AS orphan_sst_index_entries
  FROM information_schema.rocksdb_index_file_map m
 WHERE m.column_family IN (SELECT DISTINCT column_family
                             FROM information_schema.rocksdb_ddl
                            WHERE table_name LIKE 'survivor%')
   AND m.num_rows > 0
   AND NOT EXISTS (SELECT 1 FROM information_schema.rocksdb_ddl d
                    WHERE d.column_family = m.column_family
                      AND d.index_number = m.index_number);

--let $cc_n = 0
while ($cc_n < 2)
{
  --let $cc_t = survivor_$cc_n
  --disable_query_log
  --eval SELECT '$cc_t' AS t, COUNT(*) = $cc_rows AS pk_ok FROM $cc_t FORCE INDEX(PRIMARY)
  --eval SELECT '$cc_t' AS t, COUNT(*) = $cc_rows AS sk_ok FROM $cc_t FORCE INDEX(sk)
  --enable_query_log
  --inc $cc_n
}
