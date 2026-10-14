# Checks that secondary index $ccic_key of table $ccic_table holds exactly the
# rows of the primary key, over column $ccic_col.
--let $ccic_q1 = SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32($ccic_col)), 0)) FROM $ccic_table FORCE INDEX ($ccic_key)
--let $ccic_q2 = SELECT CONCAT(COUNT(*), ':', COALESCE(BIT_XOR(CRC32($ccic_col)), 0)) FROM $ccic_table FORCE INDEX (PRIMARY)
--let $ccic_r1 = `$ccic_q1`
--let $ccic_r2 = `$ccic_q2`
if ($ccic_r1 != $ccic_r2)
{
  --echo # MISMATCH $ccic_table.$ccic_key: index $ccic_r1, primary key $ccic_r2
}
if ($ccic_r1 == $ccic_r2)
{
  --echo # $ccic_table.$ccic_key matches the primary key
}
