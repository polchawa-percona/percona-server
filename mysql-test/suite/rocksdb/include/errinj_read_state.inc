# Helper of errinj_loop.inc: sets $errinj_state_now to the state of the
# test: $errinj_state (after $errinj_state_proc), followed by the full rows
# of every MyRocks table of the schema read through the primary key
# (myrocks_inv_fullrow.inc), so that a changed non-key column shows up even
# when $errinj_state checksums only the indexed columns.
--source suite/rocksdb/include/errinj_state_proc.inc
--let $errinj_state_now = `$errinj_state`
--source suite/rocksdb/include/myrocks_inv_fullrow.inc
--let $errinj_state_now = $errinj_state_now|rows:$inv_fullrow
--disable_query_log
--disable_result_log
