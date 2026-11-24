# Every index of t1 (PRIMARY, kb, kc) has the same set of rows.
--let $pk = `SELECT GROUP_CONCAT(a ORDER BY a) FROM t1 USE INDEX ()`
--let $kb = `SELECT GROUP_CONCAT(a ORDER BY a) FROM t1 FORCE INDEX (kb) WHERE b IS NOT NULL OR b IS NULL`
--let $kc = `SELECT GROUP_CONCAT(a ORDER BY a) FROM t1 FORCE INDEX (kc) WHERE c IS NOT NULL OR c IS NULL`
if ($pk == $kb) {
  if ($pk == $kc) {
    --echo # all indexes have the same rows
  }
}
if ($pk != $kb) {
  --echo # MISMATCH: PRIMARY has rows ($pk), kb has rows ($kb)
}
if ($pk != $kc) {
  --echo # MISMATCH: PRIMARY has rows ($pk), kc has rows ($kc)
}
