-- Index-by-index row counts and checksums of the MyRocks tables in a schema.
-- The same procedure runs on 8.0 (results stored in upgcheck.expected and
-- shipped in the zipped datadir) and on 8.4 after the upgrade (results in
-- upgcheck.actual), and the test compares the two tables.
--
-- For every index of every table (and once without an index hint) it reads
-- the table through that index and records COUNT(*) and SUM(CRC32(...)) over
-- the index columns plus the primary key columns, so a secondary index is
-- read index-only where possible. A "*ROW" entry reads all columns through
-- the primary key (or a full scan for a hidden primary key).

CREATE DATABASE IF NOT EXISTS upgcheck;
CREATE TABLE IF NOT EXISTS upgcheck.expected (
  db VARCHAR(64) NOT NULL, tbl VARCHAR(64) NOT NULL, idx VARCHAR(64) NOT NULL,
  cnt BIGINT, crc DECIMAL(30,0),
  PRIMARY KEY (db, tbl, idx)) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS upgcheck.actual LIKE upgcheck.expected;

DROP PROCEDURE IF EXISTS upgcheck.collect;
DELIMITER //
CREATE PROCEDURE upgcheck.collect(IN p_db VARCHAR(64), IN p_into VARCHAR(64),
                                  IN p_skip VARCHAR(1000))
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE v_tbl, v_idx VARCHAR(64);
  DECLARE v_cols, v_pk, v_all TEXT;
  DECLARE cur CURSOR FOR
    SELECT s.table_name, s.index_name
      FROM information_schema.statistics s
      JOIN information_schema.tables t
        ON t.table_schema = s.table_schema AND t.table_name = s.table_name
     WHERE s.table_schema = p_db AND t.engine = 'ROCKSDB'
       AND FIND_IN_SET(s.table_name, p_skip) = 0
     GROUP BY s.table_name, s.index_name
     ORDER BY s.table_name, s.index_name;
  DECLARE tcur CURSOR FOR
    SELECT table_name FROM information_schema.tables
     WHERE table_schema = p_db AND engine = 'ROCKSDB'
       AND FIND_IN_SET(table_name, p_skip) = 0
     ORDER BY table_name;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  SET SESSION group_concat_max_len = 65536;
  SET @del = CONCAT('DELETE FROM upgcheck.', p_into, ' WHERE db = ', QUOTE(p_db));
  PREPARE s FROM @del; EXECUTE s; DEALLOCATE PREPARE s;

  OPEN cur;
  idx_loop: LOOP
    FETCH cur INTO v_tbl, v_idx;
    IF done THEN LEAVE idx_loop; END IF;
    SELECT GROUP_CONCAT(CONCAT('COALESCE(`', column_name, '`,''~N'')')
                        ORDER BY seq_in_index SEPARATOR ',')
      INTO v_cols FROM information_schema.statistics
     WHERE table_schema = p_db AND table_name = v_tbl AND index_name = v_idx;
    SELECT GROUP_CONCAT(CONCAT('COALESCE(`', column_name, '`,''~N'')')
                        ORDER BY seq_in_index SEPARATOR ',')
      INTO v_pk FROM information_schema.statistics
     WHERE table_schema = p_db AND table_name = v_tbl AND index_name = 'PRIMARY';
    SET @q = CONCAT('INSERT INTO upgcheck.', p_into,
      ' SELECT ', QUOTE(p_db), ',', QUOTE(v_tbl), ',', QUOTE(v_idx),
      ', COUNT(*), SUM(CRC32(CONCAT_WS(''#'',', v_cols,
      IF(v_pk IS NULL OR v_idx = 'PRIMARY', '', CONCAT(',', v_pk)),
      '))) FROM `', p_db, '`.`', v_tbl, '` FORCE INDEX (`', v_idx, '`)');
    PREPARE s FROM @q; EXECUTE s; DEALLOCATE PREPARE s;
  END LOOP;
  CLOSE cur;

  SET done = 0;
  OPEN tcur;
  tbl_loop: LOOP
    FETCH tcur INTO v_tbl;
    IF done THEN LEAVE tbl_loop; END IF;
    SELECT GROUP_CONCAT(CONCAT('COALESCE(`', column_name, '`,''~N'')')
                        ORDER BY ordinal_position SEPARATOR ',')
      INTO v_all FROM information_schema.columns
     WHERE table_schema = p_db AND table_name = v_tbl;
    SET @q = CONCAT('INSERT INTO upgcheck.', p_into,
      ' SELECT ', QUOTE(p_db), ',', QUOTE(v_tbl), ',''*ROW''',
      ', COUNT(*), SUM(CRC32(CONCAT_WS(''#'',', v_all, '))) FROM `',
      p_db, '`.`', v_tbl, '`');
    PREPARE s FROM @q; EXECUTE s; DEALLOCATE PREPARE s;
  END LOOP;
  CLOSE tcur;
END//
DELIMITER ;
