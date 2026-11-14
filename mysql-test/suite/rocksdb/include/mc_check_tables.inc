# Compares every index of the tables t1, t2 and tp that exist with the
# expectations stored in mc_state (see mc_checksum.inc) and runs CHECK TABLE
# on them. Used by power_loss_mc_ddl.inc after recovery and again after the
# drop queue has drained and the final compaction has run, so damage to live
# indexes by the reclaim of dropped ones or by the compaction is caught.
--disable_query_log
--let $mc_compare = 1
--let $_mc_tables = t1 t2 tp
while ($_mc_tables) {
  --let $_mc_t = `SELECT SUBSTRING_INDEX('$_mc_tables', ' ', 1)`
  --let $_mc_tables = `SELECT IF(LOCATE(' ', '$_mc_tables'), SUBSTRING('$_mc_tables', LOCATE(' ', '$_mc_tables') + 1), '')`
  if (`SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'test' AND table_name = '$_mc_t'`) {
    --let $mc_table = $_mc_t
    --let $mc_key = $_mc_t
    --let $mc_indexes = `SELECT GROUP_CONCAT(DISTINCT index_name ORDER BY index_name <> 'PRIMARY', index_name SEPARATOR ' ') FROM information_schema.statistics WHERE table_schema = 'test' AND table_name = '$_mc_t'`
    --source suite/rocksdb/include/mc_checksum.inc
    --replace_column 2 # 3 #
    eval CHECK TABLE $_mc_t;
  }
}
