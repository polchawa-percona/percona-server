# In connection con_r, read t_live inside the transaction opened before the
# scenarios and compare with $exp_reader. Keeps the caller's $ct_* values.
--let $ctr_table = $ct_table
--let $ctr_expect = $ct_expect
--let $ctr_index = $ct_index
--let $ctr_label = $ct_label
connection con_r;
--let $ct_table = t_live
--let $ct_expect = $exp_reader
--let $ct_index = PRIMARY
--let $ct_label = $ct_label open transaction:
--source compaction_ttl_check.inc
connection default;
--let $ct_table = $ctr_table
--let $ct_expect = $ctr_expect
--let $ct_index = $ctr_index
--let $ct_label = $ctr_label
