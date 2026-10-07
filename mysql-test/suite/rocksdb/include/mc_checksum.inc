# Stores the row count and checksum of table $mc_table read through each of
# its indexes in mc_state under the prefix $mc_key, or compares them with the
# stored values when $mc_compare is 1.
#
# $mc_table    table name (t1, t2, t2ref or tp)
# $mc_indexes  space-separated index names to read through
# $mc_key      prefix of the mc_state keys
# $mc_compare  1 to compare with stored values, empty to store

--let $_mcc_list = $mc_indexes
while ($_mcc_list) {
  --let $_mcc_idx = `SELECT SUBSTRING_INDEX('$_mcc_list', ' ', 1)`
  --let $_mcc_list = `SELECT IF(LOCATE(' ', '$_mcc_list'), SUBSTRING('$_mcc_list', LOCATE(' ', '$_mcc_list') + 1), '')`
  --let $_mcc_sum = `SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b, c)))) FROM $mc_table FORCE INDEX ($_mcc_idx)`
  if (!$mc_compare) {
    eval REPLACE INTO mc_state VALUES ('$mc_key.$_mcc_idx', '$_mcc_sum');
  }
  if ($mc_compare) {
    --let $_mcc_exp = `SELECT v FROM mc_state WHERE k = '$mc_key.$_mcc_idx'`
    if ($_mcc_exp != $_mcc_sum) {
      --echo # MISMATCH $mc_table via $_mcc_idx: expected $_mcc_exp, got $_mcc_sum
    }
  }
}
