#
# power_loss_reach.inc
#
# Proves that the crashes of a power loss test were made by the crash-point
# hook and that the unsynced data was dropped in them (the "reach" evidence
# of the test). The error log of a server is not truncated between tests that
# run on the same MTR worker, so the counts are taken relative to a baseline
# read at the start of the test:
#
#   --source suite/rocksdb/include/crashpoint_site.inc
#   --let $pl_base_fired = $cp_fired
#   --let $pl_base_dropped = $cp_dropped
#   ... the test ...
#   --let $pl_want_fired = <number of crashes fired by the hook>
#   --let $pl_want_dropped = <number of them that dropped unsynced data>
#   --source suite/rocksdb/include/power_loss_reach.inc
#
# The server must be running. It dies when a count differs.
#
--source suite/rocksdb/include/crashpoint_site.inc
--let $_pr_fired = `SELECT $cp_fired - $pl_base_fired`
--let $_pr_dropped = `SELECT $cp_dropped - $pl_base_dropped`
if ($_pr_fired != $pl_want_fired) {
  --echo reach: $_pr_fired crashes were fired by the hook, expected $pl_want_fired
  --die the crash-point hook crashes do not match the test
}
if ($_pr_dropped != $pl_want_dropped) {
  --echo reach: the unsynced data was dropped in $_pr_dropped crashes, expected $pl_want_dropped
  --die the unsynced data drops do not match the test
}
--echo # reach: the expected crashes were fired by the hook, with the expected unsynced data drops
--let $_pr_fired =
--let $_pr_dropped =
--let $pl_want_fired =
--let $pl_want_dropped =
