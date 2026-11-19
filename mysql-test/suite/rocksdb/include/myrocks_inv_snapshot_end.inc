# ==== Purpose ====
#
# Invariant 5, part 2: the snapshot opened by myrocks_inv_snapshot_begin.inc
# still returns the same rows; then end the transaction. Prints nothing when
# the rows are unchanged.
#
# ==== Usage ====
#
# --source suite/rocksdb/include/myrocks_inv_snapshot_end.inc

--source suite/rocksdb/include/myrocks_inv_table_checksum.inc
if ($inv_checksum != $_invs_before) {
  --echo INVARIANT VIOLATION 5: snapshot of $inv_table changed from $_invs_before to $inv_checksum
  --inc $inv_violations
}
--disable_query_log
COMMIT;
--enable_query_log
