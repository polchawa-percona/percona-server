#
# Scratch (crash campaign): sync-point kill enumeration for DDL statements on
# shape $ce_shape.
#
--source suite/rocksdb/include/crash_enum_setup.inc
--disable_query_log
delimiter |;
CREATE PROCEDURE ce_dml_check()
BEGIN
  IF (SELECT COUNT(*) FROM information_schema.tables
      WHERE table_schema = 'test' AND table_name = 't2') > 0 THEN
    UPDATE t2 SET b = CONCAT(b, 'x') WHERE pk = 3;
    UPDATE t2 SET b = LEFT(b, CHAR_LENGTH(b) - 1) WHERE pk = 3;
    UPDATE t2 SET d = d + 1 WHERE pk = 5;
    UPDATE t2 SET d = d - 1 WHERE pk = 5;
  END IF;
END|
delimiter ;|
--enable_query_log
--let $cs_reset = CALL ce_reset('$ce_shape')
--let $cs_state_proc = CALL errinj_ddl_state()
--let $cs_state = SELECT @errinj_state
--let $cs_dml_check = CALL ce_dml_check()

--let $cs_stmt = ALTER TABLE t2 ADD INDEX kd (d), ALGORITHM=INPLACE
--let $cs_label = $ce_shape INPLACE ADD INDEX
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = ALTER TABLE t2 ADD UNIQUE INDEX ua (a), ALGORITHM=INPLACE
--let $cs_label = $ce_shape INPLACE ADD UNIQUE INDEX
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = ALTER TABLE t2 DROP INDEX kb, ALGORITHM=INPLACE
--let $cs_label = $ce_shape INPLACE DROP INDEX
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = ALTER TABLE t2 ADD COLUMN e INT NOT NULL DEFAULT 5, ADD INDEX ke (e), ALGORITHM=COPY
--let $cs_label = $ce_shape COPY ALTER
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = ALTER TABLE t2 ADD COLUMN e2 INT NOT NULL DEFAULT 5, ALGORITHM=INSTANT
if ($ce_instant_ok) {
  --let $cs_label = $ce_shape INSTANT ADD COLUMN
  --source suite/rocksdb/include/crash_sync_loop.inc
}
--let $cs_stmt = ALTER TABLE t2 RENAME INDEX kb TO kb2, ALGORITHM=INPLACE
--let $cs_label = $ce_shape RENAME INDEX
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = TRUNCATE TABLE t2
--let $cs_label = $ce_shape TRUNCATE
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = RENAME TABLE t2 TO t2_renamed, t2_renamed TO t2
--let $cs_label = $ce_shape RENAME TABLE (swap back)
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = OPTIMIZE TABLE t2
--let $cs_label = $ce_shape OPTIMIZE TABLE
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = ANALYZE TABLE t2
--let $cs_label = $ce_shape ANALYZE TABLE
--source suite/rocksdb/include/crash_sync_loop.inc
--let $cs_stmt = DROP TABLE t2
--let $cs_label = $ce_shape DROP TABLE
--source suite/rocksdb/include/crash_sync_loop.inc
DROP TABLE IF EXISTS t2;
DROP PROCEDURE ce_reset;
DROP PROCEDURE ce_reset_durable;
DROP PROCEDURE ce_dml_check;
DROP PROCEDURE errinj_ddl_reset;
DROP PROCEDURE errinj_ddl_state;
DROP PROCEDURE errinj_bulk_load;
