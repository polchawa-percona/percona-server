# Invariant checks for crash_ddl_release (release-variant item R6).
# Runs on the default connection after a server restart.
#
# Parameters:
#   $ddl_sig_old   table signature before the ALTER (columns, indexes, partitions)
#   $ddl_sig_new   table signature the ALTER produces
#   $ddl_data      "count:checksum" of the original columns before the ALTER
#   $ddl_parts     number of partitions (1 for a non-partitioned table)
#
# Sets $ddl_state to "old" or "new". Prints only lines that are the same for every
# correct outcome, so one .result covers every run.

--disable_query_log
--disable_result_log
SET SESSION group_concat_max_len = 1000000;
# ROCKSDB_DDL shows index names only after the table is opened since the restart
# (Rdb_key_def::m_name is set in Rdb_key_def::setup()), so open t1 first.
SELECT COUNT(*) FROM t1 WHERE id = 1;
--enable_result_log

--source suite/rocksdb/include/crash_ddl_release_sig.inc

# 1. The definition is either the complete old one or the complete new one.
--let $ddl_state = other
if ($ddl_sig == $ddl_sig_old)
{
  --let $ddl_state = old
}
if ($ddl_sig == $ddl_sig_new)
{
  --let $ddl_state = new
}
if ($ddl_state == other)
{
  --echo # Table definition is neither the old nor the new one: $ddl_sig
  --die Table t1 definition is not the old or the new one after the crash
}
--echo # Table definition is either the old one or the new one: yes

# 2. The data dictionary and the MyRocks dictionary list the same indexes, once per partition,
#    and the MyRocks dictionary has no leftover entries for t1 or for #sql tables.
--let $dd_idx = `SELECT GROUP_CONCAT(DISTINCT INDEX_NAME ORDER BY INDEX_NAME) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'`
--let $dd_idx_cnt = `SELECT COUNT(DISTINCT INDEX_NAME) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'`
--let $rdb_idx = `SELECT GROUP_CONCAT(DISTINCT INDEX_NAME ORDER BY INDEX_NAME) FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'`
--let $rdb_cnt = `SELECT COUNT(*) FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1'`
--let $assert_text = Data dictionary and MyRocks dictionary list the same indexes
--let $assert_cond = "$dd_idx" = "$rdb_idx" AND $rdb_cnt = $dd_idx_cnt * $ddl_parts
--source include/assert.inc
# MyRocks has no atomic DDL, so a crash during a COPY ALTER can leave the ALTER's
# intermediate #sql table in the MyRocks dictionary (not in the data dictionary). This is the
# documented behaviour for engines without atomic DDL; the documented recovery is to drop the
# #sql table by name. Check that the drop works and removes every trace of it.
--let $ddl_orphans = `SELECT COUNT(DISTINCT TABLE_NAME) FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME LIKE '#sql%'`
--let $ddl_orphans_total = `SELECT $ddl_orphans_total + $ddl_orphans`
while ($ddl_orphans)
{
  --let $ddl_orphan = `SELECT TABLE_NAME FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME LIKE '#sql%' LIMIT 1`
  --let $ddl_orphan_db = `SELECT TABLE_SCHEMA FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME = '$ddl_orphan' LIMIT 1`
  --disable_query_log
  eval DROP TABLE `$ddl_orphan_db`.`$ddl_orphan`;
  --enable_query_log
  --let $ddl_orphans = `SELECT COUNT(DISTINCT TABLE_NAME) FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME LIKE '#sql%'`
}
--let $assert_text = No #sql table is left in the MyRocks dictionary after dropping orphans by name
--let $assert_cond = [SELECT COUNT(*) FROM information_schema.ROCKSDB_DDL WHERE TABLE_NAME LIKE "#sql%"] = 0
--source include/assert.inc

# 3. Dropped or abandoned indexes are cleaned up by the background thread.
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
--let $wait_timeout = 120
--let $wait_condition = SELECT COUNT(*) = 0 FROM information_schema.ROCKSDB_GLOBAL_INFO WHERE TYPE LIKE 'DDL_%'
--source include/wait_condition.inc
--let $wait_timeout =
--echo # No index drop or index creation left unfinished: yes

# 4. The data of the original columns is unchanged.
--let $data_now = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b, pad)))) FROM t1 FORCE INDEX (PRIMARY)`
--let $assert_text = Rows and values are the same as before the ALTER
--let $assert_cond = "$data_now" = "$ddl_data"
--source include/assert.inc

# 5. CHECK TABLE is clean.
--let $check_msg = query_get_value(CHECK TABLE t1, Msg_text, 1)
--let $assert_text = CHECK TABLE reports OK
--let $assert_cond = "$check_msg" = "OK"
--source include/assert.inc

# 6. Every secondary index holds exactly the rows of the primary key.
--let $ddl_i = 0
--let $ddl_idx_bad = 0
--let $ddl_more = 1
while ($ddl_more)
{
  --let $ddl_iname = query_get_value(SELECT DISTINCT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1' AND INDEX_NAME <> 'PRIMARY' ORDER BY INDEX_NAME LIMIT 1 OFFSET $ddl_i, INDEX_NAME, 1)
  if ($ddl_iname == No such row)
  {
    --let $ddl_more = 0
  }
  if ($ddl_more)
  {
    --let $ddl_icols = `SELECT GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't1' AND INDEX_NAME = '$ddl_iname'`
    --let $via_sk = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, $ddl_icols)))) FROM t1 FORCE INDEX ($ddl_iname)`
    --let $via_pk = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, $ddl_icols)))) FROM t1 FORCE INDEX (PRIMARY)`
    if ($via_sk != $via_pk)
    {
      --echo # Index $ddl_iname differs from the primary key: $via_sk vs $via_pk
      --inc $ddl_idx_bad
    }
    --inc $ddl_i
  }
}
--let $assert_text = Every secondary index matches the primary key
--let $assert_cond = $ddl_idx_bad = 0
--source include/assert.inc

--enable_query_log
