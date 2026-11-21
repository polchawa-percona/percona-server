# Helper of errinj_loop.inc, run after the restart that follows an injected
# failure (an abort, or a restart made by the loop): no new internal DDL
# table is left, and the MyRocks invariants hold (myrocks_invariants.inc,
# which prints each violation as an INVARIANT VIOLATION row).
--let $errinj_orph_base = $errinj_orph_start,$errinj_orph_seen
--source suite/rocksdb/include/errinj_orphans_new.inc
--let $errinj_orph_when = after the restart
if ($errinj_leg_crashed) {
  --let $errinj_orph_when = after the abort
}
--let $errinj_orph_ok = 0
if ($errinj_leg_crashed) {
  if ($errinj_crash_orphans_ok) {
    --let $errinj_orph_ok = 1
  }
}
if ($errinj_orph_new) {
  if ($errinj_orph_ok) {
    --inc $errinj_crash_orphans
  }
  if (!$errinj_orph_ok) {
    --echo VIOLATION $errinj_label n=$errinj_n: internal tables left $errinj_orph_when: $errinj_orph_new
    --inc $errinj_violations
  }
}
--let $inv_skip_orphans = 1
--source suite/rocksdb/include/myrocks_invariants.inc
--let $inv_skip_orphans =
--disable_query_log
--disable_result_log
if ($inv_violations) {
  --echo VIOLATION $errinj_label n=$errinj_n: MyRocks invariants violated $errinj_orph_when (INVARIANT VIOLATION rows above)
  --inc $errinj_violations
}
