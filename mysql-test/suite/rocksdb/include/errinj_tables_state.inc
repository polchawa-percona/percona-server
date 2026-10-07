#
# Creates errinj_tables_state(tbls, outt, unfiltered), which sets
# @errinj_state to a description of the MyRocks tables in the comma-separated
# list tbls that does not depend on index ids: the partitions and their
# bounds, the index definitions and, for every partition and every index, the
# row count and a checksum of the index columns read through that index of
# that partition only. outt names an InnoDB table whose rows are added to the
# state ('' for none). With unfiltered = 1 every count is repeated with
# rocksdb_enable_ttl_read_filtering = OFF, so that rows hidden by TTL show up.
#
--disable_query_log
delimiter |;
CREATE PROCEDURE errinj_tables_state(IN tbls TEXT, IN outt VARCHAR(64),
                                     IN unfiltered INT)
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE t, p, idx VARCHAR(64);
  DECLARE cols TEXT;
  DECLARE cur CURSOR FOR
    SELECT s.table_name, IFNULL(pt.partition_name, ''), s.index_name,
           GROUP_CONCAT(CONCAT('`', s.column_name, '`')
                        ORDER BY s.seq_in_index)
    FROM information_schema.statistics s
    JOIN information_schema.partitions pt
      ON pt.table_schema = s.table_schema AND pt.table_name = s.table_name
    WHERE s.table_schema = 'test' AND FIND_IN_SET(s.table_name, tbls)
    GROUP BY s.table_name, pt.partition_name, s.index_name
    ORDER BY s.table_name, pt.partition_name, s.index_name;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  SET @errinj_state = CONCAT_WS('|',
    (SELECT GROUP_CONCAT(table_name, '/', IFNULL(partition_name, '-'), '/',
                         IFNULL(partition_description, '-')
                         ORDER BY table_name, partition_ordinal_position)
     FROM information_schema.partitions
     WHERE table_schema = 'test' AND FIND_IN_SET(table_name, tbls)),
    (SELECT GROUP_CONCAT(table_name, '.', index_name, '/', column_name, '/',
                         non_unique
                         ORDER BY table_name, index_name, seq_in_index)
     FROM information_schema.statistics
     WHERE table_schema = 'test' AND FIND_IN_SET(table_name, tbls)));
  IF @errinj_state IS NULL THEN
    SET @errinj_state = 'no_tables';
  END IF;

  OPEN cur;
  read_loop: LOOP
    FETCH cur INTO t, p, idx, cols;
    IF done THEN LEAVE read_loop; END IF;
    SET @errinj_q = CONCAT('SELECT COUNT(*), IFNULL(SUM(CRC32(CONCAT_WS(',
                           QUOTE(','), ', ', cols, ', `pk`))), 0) ',
                           'INTO @errinj_cnt, @errinj_sum FROM `', t, '`',
                           IF(p = '', '', CONCAT(' PARTITION (`', p, '`)')),
                           ' FORCE INDEX (`', idx, '`)');
    PREPARE errinj_s FROM @errinj_q;
    EXECUTE errinj_s;
    SET @errinj_state = CONCAT(@errinj_state, '|', t, '.', p, '.', idx, '=',
                               @errinj_cnt, '/', @errinj_sum);
    IF unfiltered THEN
      SET GLOBAL rocksdb_enable_ttl_read_filtering = OFF;
      EXECUTE errinj_s;
      SET GLOBAL rocksdb_enable_ttl_read_filtering = ON;
      SET @errinj_state = CONCAT(@errinj_state, '/raw=', @errinj_cnt, '/',
                                 @errinj_sum);
    END IF;
    DEALLOCATE PREPARE errinj_s;
  END LOOP;
  CLOSE cur;

  IF outt != '' THEN
    SET @errinj_q = CONCAT('SELECT COUNT(*), IFNULL(SUM(CRC32(CONCAT_WS(',
                           QUOTE(','), ', pk, a, b))), 0) ',
                           'INTO @errinj_cnt, @errinj_sum FROM `', outt, '`');
    PREPARE errinj_s FROM @errinj_q;
    EXECUTE errinj_s;
    DEALLOCATE PREPARE errinj_s;
    SET @errinj_state = CONCAT(@errinj_state, '|', outt, '=', @errinj_cnt,
                               '/', @errinj_sum);
  END IF;
  SET @errinj_state = REPLACE(@errinj_state, ' ', '_');
END|
delimiter ;|
--enable_query_log
