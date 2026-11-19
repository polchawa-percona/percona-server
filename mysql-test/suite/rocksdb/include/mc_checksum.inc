# Stores the row count and checksum of the rows of table $mc_table as seen by
# each of its indexes in mc_state under the prefix $mc_key, or compares them
# with the stored values when $mc_compare is 1.
#
# Every index is read through itself, with the columns it holds:
# - PRIMARY: the whole row;
# - ka, kb (covering: the index column and the primary key id): a covering
#   index scan of just those columns (no condition, so the index is scanned
#   completely);
# - kc (a prefix index, which never covers): the whole row, found through a
#   range over the whole domain of the indexed column (c >= '').
# The stored value is always read through PRIMARY (the same columns, from
# the same table or from an InnoDB reference table), so a secondary index
# that lost entries, or has extra ones, gives a different value than the
# primary key. Without a condition on a non-covering index the optimizer would
# read the primary key whatever FORCE INDEX says, so the access path is
# checked with EXPLAIN: the index must be the key used and, for ka and kb,
# the scan must be "Using index"; for kc the access type must be range.
#
# $mc_table    table name (t1, t2, t2ref, tp or tt)
# $mc_indexes  space-separated index names to read through
# $mc_key      prefix of the mc_state keys
# $mc_compare  1 to compare with stored values, empty to store
# $mc_store_keep  condition on the rows to store (default TRUE), for tables
#              that a DDL statement is going to remove rows from
# $mc_no_b     1 if the table has no column b (tt): PRIMARY is id, a, c

--let $_mcc_list = $mc_indexes
while ($_mcc_list) {
  --let $_mcc_idx = `SELECT SUBSTRING_INDEX('$_mcc_list', ' ', 1)`
  --let $_mcc_list = `SELECT IF(LOCATE(' ', '$_mcc_list'), SUBSTRING('$_mcc_list', LOCATE(' ', '$_mcc_list') + 1), '')`
  --let $_mcc_cols = id, a, b, c
  if ($mc_no_b) {
    --let $_mcc_cols = id, a, c
  }
  --let $_mcc_where = TRUE
  --let $_mcc_cover = 0
  if ($_mcc_idx == ka) {
    --let $_mcc_cols = id, a
    --let $_mcc_cover = 1
  }
  if ($_mcc_idx == kb) {
    --let $_mcc_cols = id, b
    --let $_mcc_cover = 1
  }
  if ($_mcc_idx == kc) {
    --let $_mcc_where = c >= ''
  }
  --let $_mcc_sel = SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32(CONCAT_WS('#', $_mcc_cols))), 0)) FROM $mc_table
  if (!$mc_compare) {
    # The reference: the same columns through PRIMARY.
    --let $_mcc_keep = TRUE
    if ($mc_store_keep) {
      --let $_mcc_keep = $mc_store_keep
    }
    --let $_mcc_sum = `$_mcc_sel FORCE INDEX (PRIMARY) WHERE ($_mcc_where) AND ($_mcc_keep)`
    eval REPLACE INTO mc_state VALUES ('$mc_key.$_mcc_idx', '$_mcc_sum');
  }
  if ($mc_compare) {
    --let $_mcc_sum = `$_mcc_sel FORCE INDEX ($_mcc_idx) WHERE $_mcc_where`
    --let $_mcc_exp = `SELECT v FROM mc_state WHERE k = '$mc_key.$_mcc_idx'`
    if ($_mcc_exp != $_mcc_sum) {
      --echo # MISMATCH $mc_table via $_mcc_idx: expected $_mcc_exp, got $_mcc_sum
    }
    # Pin the access path.
    --let $_mcc_key = query_get_value("EXPLAIN $_mcc_sel FORCE INDEX ($_mcc_idx) WHERE $_mcc_where", key, 1)
    --let $_mcc_type = query_get_value("EXPLAIN $_mcc_sel FORCE INDEX ($_mcc_idx) WHERE $_mcc_where", type, 1)
    --let $_mcc_extra = query_get_value("EXPLAIN $_mcc_sel FORCE INDEX ($_mcc_idx) WHERE $_mcc_where", Extra, 1)
    --let $_mcc_path_ok = `SELECT IF('$_mcc_idx' = 'PRIMARY', '$_mcc_type' = 'ALL' OR '$_mcc_key' = 'PRIMARY', '$_mcc_key' = '$_mcc_idx' AND IF($_mcc_cover, '$_mcc_extra' LIKE '%Using index%' AND '$_mcc_type' = 'index', '$_mcc_type' = 'range'))`
    if (!$_mcc_path_ok) {
      --echo # NOT READ THROUGH THE INDEX $mc_table $_mcc_idx: key=$_mcc_key type=$_mcc_type Extra=$_mcc_extra
    }
  }
}
