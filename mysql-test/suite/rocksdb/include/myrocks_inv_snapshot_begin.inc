# ==== Purpose ====
#
# Invariant 5, part 1: open a consistent snapshot on the current connection
# and remember the table checksum it sees. Run the scenario (for example a
# compaction or DML on other connections), then source
# myrocks_inv_snapshot_end.inc on the same connection.
#
# ==== Usage ====
#
# --let $inv_table = t1
# [--let $inv_schema = db1]
# --source suite/rocksdb/include/myrocks_inv_snapshot_begin.inc

--disable_query_log
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--enable_query_log
--source suite/rocksdb/include/myrocks_inv_table_checksum.inc
--let $_invs_before = $inv_checksum
