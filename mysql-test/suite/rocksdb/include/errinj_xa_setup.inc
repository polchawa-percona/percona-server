#
# Table and include files shared by the XA error injection tests.
#
# Every XA transaction runs on a connection of its own (see errinj_xa_run.inc)
# that is closed when the transaction is finished, and the state is checked
# on connection default.
#
--disable_query_log
CREATE TABLE tx (pk INT PRIMARY KEY, a INT NOT NULL) ENGINE=ROCKSDB;
delimiter |;
CREATE PROCEDURE errinj_xa_reset()
BEGIN
  DECLARE i INT DEFAULT 1;
  DELETE FROM tx;
  WHILE i <= 10 DO
    INSERT INTO tx VALUES (i, i);
    SET i = i + 1;
  END WHILE;
END|
delimiter ;|
--enable_query_log

--let $errinj_reset = CALL errinj_xa_reset()
--let $errinj_state_proc =
--let $errinj_state = SELECT CONCAT_WS(':', COUNT(*), IFNULL(SUM(CRC32(CONCAT_WS(',', pk, a))), 0)) FROM tx
--let $errinj_tables = tx
--let $errinj_source = suite/rocksdb/include/errinj_xa_run.inc
--let $errinj_after_restart = suite/rocksdb/include/errinj_xa_recover.inc
