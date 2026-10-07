# Checks that secondary index $ccic_key of table $ccic_table holds exactly the
# rows read through $mcc_base (the primary key unless set), over column $ccic_col.
--let $mcc_from = FORCE INDEX (PRIMARY)
if ($mcc_base)
{
  --let $mcc_from = $mcc_base
}
--let $mcc_r1 = `SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32($ccic_col)), 0)) FROM $ccic_table FORCE INDEX ($ccic_key)`
--let $mcc_r2 = `SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32($ccic_col)), 0)) FROM $ccic_table $mcc_from`
if ($mcc_r1 != $mcc_r2)
{
  --echo # MISMATCH $ccic_table.$ccic_key: index $mcc_r1, base $mcc_r2
}
if ($mcc_r1 == $mcc_r2)
{
  --echo # $ccic_table.$ccic_key matches the table
}
