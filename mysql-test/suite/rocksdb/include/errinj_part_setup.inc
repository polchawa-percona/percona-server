#
# Tables and procedures shared by the partitioning error injection tests.
#
# tp is RANGE partitioned with a column family per partition (every pk
# that is 3 modulo 4 is free), th is HASH
# partitioned and t_outp (InnoDB) receives what a statement reads. With
# $errinj_part_sk = 1 the MyRocks tables also have a secondary key.
#
--source suite/rocksdb/include/errinj_tables_state.inc
--disable_query_log
CREATE TABLE t_outp (pk INT, a INT, b VARCHAR(32)) ENGINE=InnoDB;
delimiter |;
CREATE PROCEDURE errinj_part_reset(IN with_sk INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
  DROP TABLE IF EXISTS tp, th;
  SET @sk = IF(with_sk,
    ', KEY kb (b) COMMENT ''p0_cfname=errinj_s0;p1_cfname=errinj_s1''', '');
  SET @q = CONCAT('CREATE TABLE tp (pk INT NOT NULL, a INT NOT NULL, ',
    'b VARCHAR(32) NOT NULL, PRIMARY KEY (pk) COMMENT ',
    '''p0_cfname=errinj_p0;p1_cfname=errinj_p1;p2_cfname=errinj_p2;',
    'p3_cfname=errinj_p3''', @sk, ') ENGINE=ROCKSDB ',
    'PARTITION BY RANGE (pk) (PARTITION p0 VALUES LESS THAN (10), ',
    'PARTITION p1 VALUES LESS THAN (20), PARTITION p2 VALUES LESS THAN (30), ',
    'PARTITION p3 VALUES LESS THAN (40))');
  PREPARE st FROM @q; EXECUTE st; DEALLOCATE PREPARE st;
  SET @q = CONCAT('CREATE TABLE th (pk INT NOT NULL, a INT NOT NULL, ',
    'b VARCHAR(32) NOT NULL, PRIMARY KEY (pk)',
    IF(with_sk, ', KEY kb (b)', ''),
    ') ENGINE=ROCKSDB PARTITION BY HASH (pk) PARTITIONS 4');
  PREPARE st FROM @q; EXECUTE st; DEALLOCATE PREPARE st;
  DELETE FROM t_outp;
  WHILE i < 40 DO
    IF i MOD 4 != 3 THEN
      INSERT INTO tp VALUES (i, i, CONCAT('b', i MOD 7));
    END IF;
    INSERT INTO th VALUES (i, i, CONCAT('h', i MOD 5));
    SET i = i + 1;
  END WHILE;
END|
delimiter ;|
--enable_query_log

--let $errinj_reset = CALL errinj_part_reset($errinj_part_sk)
--let $errinj_state_proc = CALL errinj_tables_state('tp,th', 't_outp', 0)
--let $errinj_state = SELECT @errinj_state
--let $errinj_tables = tp,th
--let $errinj_source = suite/rocksdb/include/errinj_stmt.inc
--let $errinj_after_restart = suite/rocksdb/include/errinj_noop.inc
