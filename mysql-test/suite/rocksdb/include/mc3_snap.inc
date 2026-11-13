# Records per-index counts and checksums of $mc3_t (shape $mc3_shape) under
# label $mc3_label.
--disable_query_log
if ($mc3_shape == live) {
  eval CALL mc3.snap('$mc3_label', '$mc3_t', 'id, a, u, CRC32(b)',
                     'PRIMARY,ka,ku');
}
if ($mc3_shape == hid) {
  eval CALL mc3.snap('$mc3_label', '$mc3_t', 'a, c', ',kc');
}
if ($mc3_shape == pidx) {
  eval CALL mc3.snap('$mc3_label', '$mc3_t', 'i, j, k, l', 'PRIMARY,ik1,ik2');
}
--enable_query_log
