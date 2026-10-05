#
# Compare a single-part functional index with the table: the rows found
# through the index for "$fi_expr IS NOT NULL" must be the rows with a
# non-NULL $fi_expr found without the index.
#
# Parameters:
#   $fi_table  table name
#   $fi_index  functional index name
#   $fi_expr   the indexed expression, written without backticks
#   $fi_pk     primary key column
#
--disable_query_log
--let $_fic_via_index= `SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32(CONCAT_WS('#', $fi_pk, $fi_expr))), 0)) FROM $fi_table FORCE INDEX ($fi_index) WHERE $fi_expr IS NOT NULL`
--let $_fic_via_pk= `SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32(CONCAT_WS('#', $fi_pk, $fi_expr))), 0)) FROM $fi_table IGNORE INDEX ($fi_index) WHERE $fi_expr IS NOT NULL`
--let $_fic_ok= `SELECT '$_fic_via_index' = '$_fic_via_pk'`
if (!$_fic_ok) {
  --echo # index $fi_index: $_fic_via_index, without the index: $_fic_via_pk
}
--enable_query_log
--echo # $fi_table.$fi_index matches the table: $_fic_ok
