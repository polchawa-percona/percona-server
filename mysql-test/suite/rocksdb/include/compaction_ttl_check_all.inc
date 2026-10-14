# Check every index of the compaction_ttl_* tables against expected counts.
# Parameters:
#   $exp_col, $exp_hid, $exp_part, $exp_plain   expected rows per table
#   $ct_label                                   text printed in the result
--let $ct_table = t_col
--let $ct_expect = $exp_col
--let $ct_index = PRIMARY
--source compaction_ttl_check.inc
--let $ct_index = ub
--source compaction_ttl_check.inc
--let $ct_index = kc
--source compaction_ttl_check.inc
--let $ct_table = t_hid
--let $ct_expect = $exp_hid
--let $ct_index = PRIMARY
--source compaction_ttl_check.inc
--let $ct_index = kb
--source compaction_ttl_check.inc
--let $ct_table = t_part
--let $ct_expect = $exp_part
--let $ct_index = PRIMARY
--source compaction_ttl_check.inc
--let $ct_index = kb
--source compaction_ttl_check.inc
--let $ct_table = t_plain
--let $ct_expect = $exp_plain
--let $ct_index = PRIMARY
--source compaction_ttl_check.inc
--let $ct_index = kb
--source compaction_ttl_check.inc
