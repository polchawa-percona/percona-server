#
# Tables and procedures shared by the DML error injection tests.
#
# t1 is the MyRocks table under test: a primary key, a unique secondary key and
# two non-unique secondary keys. t_out is an InnoDB table that receives the
# rows a statement reads from t1, so that a read that silently returns fewer
# rows shows up in the state. errinj_state is the state query used by
# errinj_loop.inc: per-index row counts and checksums of t1, plus a checksum
# of t_out.
#

--disable_query_log
CREATE TABLE t1 (
  pk INT PRIMARY KEY,
  a INT NOT NULL,
  b VARCHAR(32) NOT NULL,
  c INT NOT NULL,
  UNIQUE KEY ua (a),
  KEY kb (b),
  KEY kc (c)
) ENGINE=ROCKSDB;

CREATE TABLE t_out (pk INT, a INT, b VARCHAR(32), c INT) ENGINE=InnoDB;

delimiter |;
CREATE PROCEDURE errinj_reset()
BEGIN
  DECLARE i INT DEFAULT 1;
  -- READ COMMITTED: at REPEATABLE READ MyRocks rejects INSERT ... SELECT
  -- from a MyRocks table (gap locks are not supported).
  SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
  DELETE FROM t1;
  DELETE FROM t_out;
  WHILE i <= 20 DO
    INSERT INTO t1 VALUES (i, i, CONCAT('b', i MOD 7), i * 10);
    SET i = i + 1;
  END WHILE;
END|

CREATE PROCEDURE errinj_tx_rr()
BEGIN
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN ROLLBACK; RESIGNAL; END;
  SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ;
  START TRANSACTION;
  SELECT c INTO @c FROM t1 WHERE pk = 3 FOR UPDATE;
  INSERT INTO t1 VALUES (31, 31, 'tx', 310), (32, 32, 'tx', 320);
  UPDATE t1 SET c = c + 1, b = 'upd' WHERE pk = 3;
  DELETE FROM t1 WHERE pk = 4;
  SELECT COUNT(*), SUM(c) INTO @n, @s FROM t1 WHERE pk BETWEEN 1 AND 10;
  INSERT INTO t_out VALUES (@n, @s, 'rr', 0);
  COMMIT;
END|

CREATE PROCEDURE errinj_tx_rc()
BEGIN
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN ROLLBACK; RESIGNAL; END;
  SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
  START TRANSACTION;
  UPDATE t1 SET a = a + 100 WHERE pk BETWEEN 5 AND 8;
  INSERT INTO t1 VALUES (41, 41, 'rc', 410);
  DELETE FROM t1 WHERE b = 'b2';
  INSERT INTO t_out SELECT * FROM t1 FORCE INDEX (kb) WHERE b >= 'b4';
  COMMIT;
END|
delimiter ;|
--enable_query_log

--let $errinj_reset = CALL errinj_reset()
--let $errinj_tables = t1
--let $errinj_state = SELECT CONCAT_WS(':', (SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY)), (SELECT COUNT(*) FROM t1 FORCE INDEX (ua)), (SELECT COUNT(*) FROM t1 FORCE INDEX (kb)), (SELECT COUNT(*) FROM t1 FORCE INDEX (kc)), (SELECT IFNULL(SUM(CRC32(CONCAT_WS(',', pk, a, b, c))), 0) FROM t1 FORCE INDEX (PRIMARY)), (SELECT IFNULL(SUM(CRC32(CONCAT_WS(',', a, pk))), 0) FROM t1 FORCE INDEX (ua)), (SELECT IFNULL(SUM(CRC32(CONCAT_WS(',', b, pk))), 0) FROM t1 FORCE INDEX (kb)), (SELECT IFNULL(SUM(CRC32(CONCAT_WS(',', c, pk))), 0) FROM t1 FORCE INDEX (kc)), (SELECT COUNT(*) FROM t_out), (SELECT IFNULL(SUM(CRC32(CONCAT_WS(',', pk, a, b, c))), 0) FROM t_out))
