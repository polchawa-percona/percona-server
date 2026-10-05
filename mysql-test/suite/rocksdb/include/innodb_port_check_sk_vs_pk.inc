# Check that a secondary index of a MyRocks table holds exactly the rows of the table.
#
# Reads the indexed columns once through the index (index-only scan) and once without the
# index (primary key / table scan), and compares row count and a checksum over the indexed
# columns. Prints one line: the index name and whether both reads agree.
#
# Parameters:
#   $pk_check_table  table name (in the current database, plain identifier)
#   $pk_check_index  secondary index name (plain identifier)
#
--disable_query_log
--let $_pkc_cols= `SELECT GROUP_CONCAT(column_name ORDER BY seq_in_index) FROM information_schema.statistics WHERE table_schema = DATABASE() AND table_name = '$pk_check_table' AND index_name = '$pk_check_index' AND column_name IS NOT NULL`
--let $_pkc_via_index= `SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32(CONCAT_WS('#', $_pkc_cols))), 0)) FROM $pk_check_table FORCE INDEX ($pk_check_index)`
--let $_pkc_via_pk= `SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32(CONCAT_WS('#', $_pkc_cols))), 0)) FROM $pk_check_table IGNORE INDEX ($pk_check_index)`
--let $_pkc_ok= `SELECT '$_pkc_via_index' = '$_pkc_via_pk'`
if (!$_pkc_ok)
{
  --echo # index $pk_check_index: $_pkc_via_index, without the index: $_pkc_via_pk
}
--enable_query_log
--echo # $pk_check_table.$pk_check_index matches the table: $_pkc_ok
