#
# Procedures for the data dictionary error injection tests of DDL.
#
# errinj_dict_reset(extra) drops test.t2, t3, tp and tc and recreates t2
# (30 rows); extra = 1 also creates the partitioned table tp, extra = 2 the
# table tc whose indexes live in their own column families.
#
# errinj_dict_state() sets @errinj_state to a description of every table of
# schema test that does not depend on index ids: the tables, columns, indexes
# and partitions in the server data dictionary, the tables, partitions,
# index types and column families in the MyRocks data dictionary (index
# names are left out: they are empty until a table is opened) (so that a
# #sql leftover or a missing entry shows up), and for every index the row
# count and a checksum read through that index only.
#

--disable_query_log
delimiter |;
CREATE PROCEDURE errinj_dict_reset(extra INT)
BEGIN
  DECLARE i INT DEFAULT 1;
  DROP TABLE IF EXISTS t2, t3, tp, tc;
  CREATE TABLE t2 (
    pk INT PRIMARY KEY,
    a INT NOT NULL,
    b VARCHAR(32) NOT NULL,
    d INT NOT NULL,
    KEY kb (b),
    KEY kd (d)
  ) ENGINE=ROCKSDB;
  IF extra = 1 THEN
    CREATE TABLE tp (pk INT PRIMARY KEY, a INT NOT NULL, KEY ka (a))
      ENGINE=ROCKSDB PARTITION BY HASH (pk) PARTITIONS 3;
  END IF;
  IF extra = 2 THEN
    CREATE TABLE tc (
      pk INT NOT NULL,
      a INT NOT NULL,
      PRIMARY KEY (pk) COMMENT 'cfname=cf_errinj',
      KEY ka (a) COMMENT 'cfname=rev:cf_errinj2'
    ) ENGINE=ROCKSDB;
  END IF;
  WHILE i <= 30 DO
    INSERT INTO t2 VALUES (i, i, CONCAT('b', i MOD 7), 300 - i);
    IF extra = 1 THEN
      INSERT INTO tp VALUES (i, i);
    END IF;
    IF extra = 2 THEN
      INSERT INTO tc VALUES (i, i);
    END IF;
    SET i = i + 1;
  END WHILE;
END|

CREATE PROCEDURE errinj_dict_state()
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE tbl VARCHAR(64);
  DECLARE idx VARCHAR(64);
  DECLARE cols TEXT;
  DECLARE cur CURSOR FOR
    SELECT table_name, index_name,
           GROUP_CONCAT(CONCAT('`', column_name, '`') ORDER BY seq_in_index)
    FROM information_schema.statistics
    WHERE table_schema = 'test'
    GROUP BY table_name, index_name ORDER BY table_name, index_name;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  SET @errinj_state = CONCAT_WS('|',
    (SELECT GROUP_CONCAT(table_name ORDER BY table_name)
     FROM information_schema.tables WHERE table_schema = 'test'),
    (SELECT GROUP_CONCAT(table_name, '.', column_name, ' ', column_type, ' ',
                         IFNULL(column_default, 'NULL')
                         ORDER BY table_name, ordinal_position)
     FROM information_schema.columns WHERE table_schema = 'test'),
    (SELECT GROUP_CONCAT(table_name, '.', index_name, '/', column_name, '/',
                         non_unique ORDER BY table_name, index_name,
                         seq_in_index)
     FROM information_schema.statistics WHERE table_schema = 'test'),
    (SELECT GROUP_CONCAT(table_name, '.', partition_name
                         ORDER BY table_name, partition_name)
     FROM information_schema.partitions
     WHERE table_schema = 'test' AND partition_name IS NOT NULL),
    (SELECT GROUP_CONCAT(table_name, '.', IFNULL(partition_name, ''), '.',
                         index_type, '@', cf
                         ORDER BY table_name, partition_name, index_type, cf)
     FROM information_schema.rocksdb_ddl WHERE table_schema = 'test'));
  IF @errinj_state IS NULL OR @errinj_state = '' THEN
    SET @errinj_state = 'no_table';
  END IF;

  OPEN cur;
  read_loop: LOOP
    FETCH cur INTO tbl, idx, cols;
    IF done THEN LEAVE read_loop; END IF;
    BEGIN
      DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
        SET @errinj_cnt = 'unreadable', @errinj_sum = '';
      SET @errinj_q = CONCAT('SELECT COUNT(*), IFNULL(SUM(CRC32(CONCAT_WS(',
                             QUOTE(','), ', ', cols, ', `pk`))), 0) ',
                             'INTO @errinj_cnt, @errinj_sum FROM `', tbl,
                             '` FORCE INDEX (`', idx, '`)');
      PREPARE errinj_s FROM @errinj_q;
      EXECUTE errinj_s;
      DEALLOCATE PREPARE errinj_s;
    END;
    SET @errinj_state = CONCAT(@errinj_state, '|', tbl, '.', idx, '=',
                               @errinj_cnt, '/', @errinj_sum);
  END LOOP;
  CLOSE cur;
  SET @errinj_state = REPLACE(@errinj_state, ' ', '_');
END|
delimiter ;|
--enable_query_log

--let $errinj_state_proc = CALL errinj_dict_state()
--let $errinj_state = SELECT @errinj_state
--let $errinj_crash = 1
