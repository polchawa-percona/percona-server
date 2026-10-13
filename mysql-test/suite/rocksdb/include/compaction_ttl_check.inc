# Compare the number of rows read through one index with the expected count.
# Parameters:
#   $ct_table   table name
#   $ct_index   index name (PRIMARY or a secondary index)
#   $ct_expect  expected row count
#   $ct_label   text printed in the result
--let $ct_got = `SELECT COUNT(*) FROM $ct_table FORCE INDEX ($ct_index)`
if ($ct_got == $ct_expect)
{
  --echo $ct_label $ct_table.$ct_index: ok
}
if ($ct_got != $ct_expect)
{
  --echo $ct_label $ct_table.$ct_index: MISMATCH got $ct_got expected $ct_expect
}
