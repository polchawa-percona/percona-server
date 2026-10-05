#
# Sync the replica and compare every test table between source and replica,
# then check on the replica that each secondary index has as many entries as
# the primary key.
#
# Parameters:
#   $step  label printed in the result
#
--echo # Check after: $step
--source include/rpl/sync_to_replica.inc
--let $diff_tables = master:t, slave:t
--source include/diff_tables.inc
--let $diff_tables = master:p, slave:p
--source include/diff_tables.inc
--let $diff_tables = master:n, slave:n
--source include/diff_tables.inc
--let $diff_tables = master:w, slave:w
--source include/diff_tables.inc
--let $sk_mismatch = `SELECT (SELECT COUNT(*) FROM t) <> (SELECT COUNT(*) FROM t FORCE INDEX (kb)) OR (SELECT COUNT(*) FROM p) <> (SELECT COUNT(*) FROM p FORCE INDEX (kpb)) OR (SELECT COUNT(*) FROM w) <> (SELECT COUNT(*) FROM w FORCE INDEX (kw))`
if ($sk_mismatch)
{
  --echo Secondary index row count differs from the primary key on the replica
  SELECT COUNT(*) FROM t; SELECT COUNT(*) FROM t FORCE INDEX (kb);
  SELECT COUNT(*) FROM p; SELECT COUNT(*) FROM p FORCE INDEX (kpb);
  SELECT COUNT(*) FROM w; SELECT COUNT(*) FROM w FORCE INDEX (kw);
  --die Secondary index mismatch on the replica
}
--source include/rpl/connection_source.inc
