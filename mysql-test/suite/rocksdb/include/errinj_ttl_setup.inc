#
# Tables and procedures shared by the TTL error injection tests.
#
# tt_h has a hidden TTL timestamp, tt_c an explicit TTL column ts. Both have
# rows 1-5 expired (written 3600 seconds ago with a TTL of 3000 seconds)
# and rows 6-15 not expired. With $errinj_ttl_sk = 1 they also have a
# secondary key. The state counts every index with TTL read filtering on, so
# that expired rows that come back and unexpired rows that vanish both show
# up. Counts without filtering are not compared: any compaction, including
# background ones, may remove expired rows at any time. t_outt (InnoDB) receives what a statement reads.
#
--source suite/rocksdb/include/errinj_tables_state.inc
--disable_query_log
CREATE TABLE t_outt (pk INT, a INT, b VARCHAR(32)) ENGINE=InnoDB;
delimiter |;
CREATE PROCEDURE errinj_ttl_reset(IN with_sk INT)
BEGIN
  DECLARE i INT DEFAULT 1;
  SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
  DROP TABLE IF EXISTS tt_h, tt_c;
  SET @q = CONCAT('CREATE TABLE tt_h (pk INT NOT NULL PRIMARY KEY, ',
    'a INT NOT NULL, b VARCHAR(32) NOT NULL',
    IF(with_sk, ', KEY kb (b)', ''),
    ') ENGINE=ROCKSDB COMMENT=''ttl_duration=3000;''');
  PREPARE st FROM @q; EXECUTE st; DEALLOCATE PREPARE st;
  SET @q = CONCAT('CREATE TABLE tt_c (pk INT NOT NULL PRIMARY KEY, ',
    'a INT NOT NULL, b VARCHAR(32) NOT NULL, ts BIGINT UNSIGNED NOT NULL',
    IF(with_sk, ', KEY ka (a)', ''),
    ') ENGINE=ROCKSDB COMMENT=''ttl_duration=3000;ttl_col=ts;''');
  PREPARE st FROM @q; EXECUTE st; DEALLOCATE PREPARE st;
  DELETE FROM t_outt;
  SET GLOBAL rocksdb_debug_ttl_rec_ts = -3600;
  WHILE i <= 5 DO
    INSERT INTO tt_h VALUES (i, i, CONCAT('b', i MOD 4));
    SET i = i + 1;
  END WHILE;
  SET GLOBAL rocksdb_debug_ttl_rec_ts = 0;
  WHILE i <= 15 DO
    INSERT INTO tt_h VALUES (i, i, CONCAT('b', i MOD 4));
    SET i = i + 1;
  END WHILE;
  SET i = 1;
  WHILE i <= 15 DO
    INSERT INTO tt_c VALUES (i, i * 10, CONCAT('c', i MOD 3),
      UNIX_TIMESTAMP() - IF(i <= 5, 3600, 0));
    SET i = i + 1;
  END WHILE;
END|
delimiter ;|
--enable_query_log

--let $errinj_reset = CALL errinj_ttl_reset($errinj_ttl_sk)
--let $errinj_state_proc = CALL errinj_tables_state('tt_h,tt_c', 't_outt', 0)
--let $errinj_state = SELECT @errinj_state
--let $errinj_tables = tt_h,tt_c
--let $errinj_source = suite/rocksdb/include/errinj_stmt.inc
--let $errinj_after_restart = suite/rocksdb/include/errinj_noop.inc
