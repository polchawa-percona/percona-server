# Helper of errinj_loop.inc: sets $errinj_orph_new to the internal DDL tables
# (#sql*, #truncate_tmp#*, see myrocks_inv_orphans.inc) of the schema that
# exist now and are not in the comma list $errinj_orph_base, or to 0 when
# there is none. $errinj_orph_now is the full list. With $errinj_leg_crashed
# = 1 (after an abort) the entries of $inv_orphans_abort_ok are not new
# (myrocks_inv_orphans.inc); $errinj_orph_abort_n counts the legs that left
# such an entry that was not there before the leg.
--source suite/rocksdb/include/myrocks_inv_orphans.inc
--disable_query_log
--disable_result_log
--let $errinj_orph_now = $inv_orphans
--let $errinj_orph_new = 0
if ($inv_orphans != none) {
  --eval SET @errinj_orph_now = '$inv_orphans', @errinj_orph_base = '$errinj_orph_base'
  if ($errinj_leg_crashed) {
    if ($inv_orphans_abort_ok != none) {
      --eval SET @errinj_orph_ok = '$inv_orphans_abort_ok'
      --let $errinj_orph_ok_new = `SELECT COUNT(*) FROM JSON_TABLE(CONCAT('["', REPLACE(@errinj_orph_ok, ',', '","'), '"]'), '$[*]' COLUMNS (n VARCHAR(512) PATH '$')) j WHERE NOT FIND_IN_SET(j.n, @errinj_orph_base)`
      if ($errinj_orph_ok_new) {
        --inc $errinj_orph_abort_n
      }
      SET @errinj_orph_base = CONCAT_WS(',', @errinj_orph_base, @errinj_orph_ok);
    }
  }
  --let $errinj_orph_new = `SELECT IFNULL(GROUP_CONCAT(j.n ORDER BY j.n), 0) FROM JSON_TABLE(CONCAT('["', REPLACE(@errinj_orph_now, ',', '","'), '"]'), '$[*]' COLUMNS (n VARCHAR(512) PATH '$')) j WHERE NOT FIND_IN_SET(j.n, @errinj_orph_base)`
}
