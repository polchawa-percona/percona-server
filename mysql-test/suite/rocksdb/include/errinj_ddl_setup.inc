#
# Procedures shared by the DDL error injection tests.
#
# errinj_ddl_reset() recreates test.t2 with 30 rows. errinj_ddl_state() sets
# @errinj_state to a description of t2 that does not depend on index ids:
# the column and index definitions from the data dictionary, the number of
# MyRocks dictionary entries of the table, and for every index the row count
# and a checksum of the index columns read through that index only.
#

--disable_query_log
delimiter |;
CREATE PROCEDURE errinj_ddl_reset()
BEGIN
  DECLARE i INT DEFAULT 1;
  DROP TABLE IF EXISTS t2;
  CREATE TABLE t2 (
    pk INT PRIMARY KEY,
    a INT NOT NULL,
    b VARCHAR(32) NOT NULL,
    d INT NOT NULL,
    KEY kb (b)
  ) ENGINE=ROCKSDB;
  WHILE i <= 30 DO
    INSERT INTO t2 VALUES (i, i, CONCAT('b', i MOD 7), 300 - i);
    SET i = i + 1;
  END WHILE;
END|

CREATE PROCEDURE errinj_ddl_state()
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE idx VARCHAR(64);
  DECLARE cols TEXT;
  DECLARE cur CURSOR FOR
    SELECT index_name,
           GROUP_CONCAT(CONCAT('`', column_name, '`') ORDER BY seq_in_index)
    FROM information_schema.statistics
    WHERE table_schema = 'test' AND table_name = 't2'
    GROUP BY index_name ORDER BY index_name;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  SET @errinj_state = CONCAT_WS('|',
    (SELECT GROUP_CONCAT(column_name, ' ', column_type, ' ',
                         IFNULL(column_default, 'NULL')
                         ORDER BY ordinal_position)
     FROM information_schema.columns
     WHERE table_schema = 'test' AND table_name = 't2'),
    (SELECT GROUP_CONCAT(index_name, '/', column_name, '/', non_unique
                         ORDER BY index_name, seq_in_index)
     FROM information_schema.statistics
     WHERE table_schema = 'test' AND table_name = 't2'),
    (SELECT COUNT(*) FROM information_schema.rocksdb_ddl
     WHERE table_schema = 'test' AND table_name = 't2'));
  IF @errinj_state IS NULL OR @errinj_state = '0' THEN
    SET @errinj_state = 'no_table';
  END IF;

  OPEN cur;
  read_loop: LOOP
    FETCH cur INTO idx, cols;
    IF done THEN LEAVE read_loop; END IF;
    SET @errinj_q = CONCAT('SELECT COUNT(*), IFNULL(SUM(CRC32(CONCAT_WS(',
                           QUOTE(','), ', ', cols, ', `pk`))), 0) ',
                           'INTO @errinj_cnt, @errinj_sum FROM t2 ',
                           'FORCE INDEX (`', idx, '`)');
    PREPARE errinj_s FROM @errinj_q;
    EXECUTE errinj_s;
    DEALLOCATE PREPARE errinj_s;
    SET @errinj_state = CONCAT(@errinj_state, '|', idx, '=', @errinj_cnt,
                               '/', @errinj_sum);
  END LOOP;
  CLOSE cur;
  SET @errinj_state = REPLACE(@errinj_state, ' ', '_');
END|

CREATE PROCEDURE errinj_bulk_load()
BEGIN
  DECLARE i INT DEFAULT 31;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION
  BEGIN
    SET SESSION rocksdb_bulk_load = 0;
    RESIGNAL;
  END;
  SET SESSION rocksdb_bulk_load = 1;
  WHILE i <= 60 DO
    INSERT INTO t2 VALUES (i, i, CONCAT('bl', i MOD 5), 300 - i);
    SET i = i + 1;
  END WHILE;
  SET SESSION rocksdb_bulk_load = 0;
END|
delimiter ;|
--enable_query_log

--let $errinj_reset = CALL errinj_ddl_reset()
--let $errinj_state_proc = CALL errinj_ddl_state()
--let $errinj_state = SELECT @errinj_state
--let $errinj_tables =
--let $errinj_source = suite/rocksdb/include/errinj_stmt.inc
--let $errinj_after_restart = suite/rocksdb/include/errinj_noop.inc
