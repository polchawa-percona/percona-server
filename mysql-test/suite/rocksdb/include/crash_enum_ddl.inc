#
# Scratch (crash campaign): crash at the N-th RocksDB call of each class
# (before and after the call) for every DDL workload on shape $ce_shape.
#
--source suite/rocksdb/include/crash_enum_setup.inc
--disable_query_log
delimiter |;
CREATE PROCEDURE ce_dml_check()
BEGIN
  IF (SELECT COUNT(*) FROM information_schema.tables
      WHERE table_schema = 'test' AND table_name = 't2') > 0 THEN
    REPLACE INTO t2 SELECT * FROM t2 ORDER BY pk LIMIT 1;
    UPDATE t2 SET d = d + 1 WHERE pk = 5;
    UPDATE t2 SET d = d - 1 WHERE pk = 5;
  END IF;
END|
delimiter ;|
--enable_query_log
--let $errinj_reset = CALL ce_reset('$ce_shape')
if ($ce_power) {
  --let $errinj_reset = CALL ce_reset_durable('$ce_shape')
  if (!$ce_statuses) {
    --let $ce_statuses = POWER_LOSS_BEFORE,POWER_LOSS_AFTER
  }
}
--let $errinj_crash = 1
--let $ce_invariants = 1
--let $ce_dml_check = CALL ce_dml_check()
--let $errinj_bulk_load = 2
if ($ce_power) {
  # Bulk load under power loss: power_loss_bulk_load, PS-11696.
  --let $errinj_bulk_load = 0
}
if ($ce_no_bulk) {
  --let $errinj_bulk_load = 0
}
if (!$ce_classes) {
  --let $ce_classes = DICT,COMMIT,INGEST,SST
}
if (!$ce_statuses) {
  --let $ce_statuses = CRASH_BEFORE,CRASH_AFTER
}
--let $ce_c = $ce_classes
while ($ce_c) {
  --let $errinj_class = `SELECT SUBSTRING_INDEX('$ce_c', ',', 1)`
  --let $ce_c = `SELECT IF(LOCATE(',', '$ce_c') > 0, SUBSTRING('$ce_c', LOCATE(',', '$ce_c') + 1), '')`
  --let $ce_s = $ce_statuses
  while ($ce_s) {
    --let $errinj_status = `SELECT SUBSTRING_INDEX('$ce_s', ',', 1)`
    --let $ce_s = `SELECT IF(LOCATE(',', '$ce_s') > 0, SUBSTRING('$ce_s', LOCATE(',', '$ce_s') + 1), '')`
    --source suite/rocksdb/include/errinj_ddl_workloads.inc
  }
}
DROP TABLE IF EXISTS t2;
DROP PROCEDURE ce_reset;
DROP PROCEDURE ce_reset_durable;
DROP PROCEDURE ce_dml_check;
DROP PROCEDURE errinj_ddl_reset;
DROP PROCEDURE errinj_ddl_state;
DROP PROCEDURE errinj_bulk_load;
