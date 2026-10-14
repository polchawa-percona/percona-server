# ==== Purpose ====
#
# Invariant 2: a MyRocks table holds the same data as an InnoDB mirror that
# was fed the same statements. Prints nothing when they match.
#
# ==== Usage ====
#
# --let $inv_table = t1          # MyRocks table
# --let $inv_mirror = t1_innodb  # InnoDB mirror, same columns in the same order
# [--let $inv_schema = db1]      # default: DATABASE(), used for both tables
# --source suite/rocksdb/include/myrocks_inv_mirror_compare.inc

--let $_invm_table = $inv_table
--source suite/rocksdb/include/myrocks_inv_table_checksum.inc
--let $_invm_a = $inv_checksum
--let $inv_table = $inv_mirror
--source suite/rocksdb/include/myrocks_inv_table_checksum.inc
--let $_invm_b = $inv_checksum
--let $inv_table = $_invm_table
if ($_invm_a != $_invm_b) {
  --echo INVARIANT VIOLATION 2: $inv_table ($_invm_a) differs from mirror $inv_mirror ($_invm_b)
  --inc $inv_violations
}
