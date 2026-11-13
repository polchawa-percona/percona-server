#
# crashpoint_state.inc
#
# Creates the procedure crashpoint_state(pkcol), which fills the user
# variable @crashpoint_state with a description of the state of every
# table of schema cp (the schema the crash-point tests work in; it is
# dropped and recreated before every window, see crashpoint_loop.inc)
# that is independent of index ids:
#   - the tables and their columns in the server data dictionary,
#   - the indexes in the server data dictionary,
#   - the tables and index definitions in the MyRocks data dictionary,
#     except the internal #sql% DDL temp tables (leftovers of those are
#     checked separately by crashpoint_loop.inc, which counts them),
#   - for the primary key: the row count and a checksum over every
#     column of every row,
#   - for every secondary index two row counts and checksums: one over
#     the index entries only (index columns + pkcol, a covering read)
#     and one over every column of every row read in the order of that
#     index (the index entry is followed by the primary key lookup, so a
#     dangling or missing entry changes the count).
# Server DD tables that are hidden (hidden = DDL, e.g. a #sql leftover
# that exists only in the server dictionary) are not visible here.
# The driver compares the value after a crash with the values before and
# after an uninjected run: it must be one of the two, never a mix.

--disable_query_log
--disable_warnings
delimiter |;
DROP PROCEDURE IF EXISTS crashpoint_run|
CREATE PROCEDURE crashpoint_run(q TEXT)
BEGIN
  SET @crashpoint_rq = q;
  BEGIN
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
      GET DIAGNOSTICS CONDITION 1 @crashpoint_err_prep = MESSAGE_TEXT;
      SET @crashpoint_cnt = 'unreadable', @crashpoint_sum = '';
    END;
    PREPARE crashpoint_s FROM @crashpoint_rq;
  END;
  BEGIN
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
      GET DIAGNOSTICS CONDITION 1 @crashpoint_err_exec = MESSAGE_TEXT;
      SET @crashpoint_cnt = 'unreadable', @crashpoint_sum = '';
    END;
    EXECUTE crashpoint_s;
  END;
  BEGIN
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
    BEGIN
      GET DIAGNOSTICS CONDITION 1 @crashpoint_err_dealloc = MESSAGE_TEXT;
    END;
    DEALLOCATE PREPARE crashpoint_s;
  END;
END|
DROP PROCEDURE IF EXISTS crashpoint_state|
CREATE PROCEDURE crashpoint_state(pkcol VARCHAR(64))
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE tbl VARCHAR(64);
  DECLARE idx VARCHAR(64);
  DECLARE cols TEXT;
  DECLARE allcols TEXT;
  DECLARE ordcols TEXT;
  DECLARE cur CURSOR FOR
    SELECT table_name, index_name,
           GROUP_CONCAT(CONCAT('IFNULL(`', column_name, '`, ''<NULL>'')')
                        ORDER BY seq_in_index),
           GROUP_CONCAT(CONCAT('`', column_name, '`') ORDER BY seq_in_index)
    FROM information_schema.statistics
    WHERE table_schema = 'cp'
    GROUP BY table_name, index_name ORDER BY table_name, index_name;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  # The error variables are session variables: reset them so that a value
  # left by an earlier call cannot leak into this fingerprint.
  SET @crashpoint_err_prep = NULL, @crashpoint_err_exec = NULL,
      @crashpoint_err_dealloc = NULL;
  SET SESSION group_concat_max_len = 1000000;
  SET @crashpoint_state = CONCAT_WS('|',
    (SELECT IFNULL(GROUP_CONCAT(table_name ORDER BY table_name), '')
     FROM information_schema.tables
     WHERE table_schema = 'cp' AND table_type = 'BASE TABLE'),
    (SELECT IFNULL(GROUP_CONCAT(table_name, '.', column_name, ' ',
                         column_type ORDER BY table_name, ordinal_position),
                   '')
     FROM information_schema.columns WHERE table_schema = 'cp'),
    (SELECT IFNULL(GROUP_CONCAT(table_name, '.', index_name, '/',
                         column_name, '/', non_unique
                         ORDER BY table_name, index_name, seq_in_index),
                   '')
     FROM information_schema.statistics WHERE table_schema = 'cp'),
    (SELECT IFNULL(GROUP_CONCAT(table_name, '.', IFNULL(partition_name, ''),
                         '.', index_type, '@', cf
                         ORDER BY table_name, partition_name,
                                  index_type, cf),
                   '')
     FROM information_schema.rocksdb_ddl
     WHERE table_schema = 'cp' AND table_name NOT LIKE '#sql%'));
  IF @crashpoint_state IS NULL OR @crashpoint_state = '' THEN
    SET @crashpoint_state = 'no_tables';
  END IF;

  OPEN cur;
  read_loop: LOOP
    FETCH cur INTO tbl, idx, cols, ordcols;
    IF done THEN LEAVE read_loop; END IF;
    SELECT GROUP_CONCAT(CONCAT('IFNULL(`', column_name, '`, ''<NULL>'')')
                        ORDER BY ordinal_position)
      INTO allcols
      FROM information_schema.columns
      WHERE table_schema = 'cp' AND table_name = tbl;
    # Part 1: the index entries (a covering read for a secondary index,
    # which holds the index columns and the primary key); for the primary
    # key, every column of every row.
    IF idx = 'PRIMARY' THEN
      SET @crashpoint_q = CONCAT('SELECT COUNT(*), ',
                                 'IFNULL(SUM(CRC32(CONCAT_WS(', QUOTE(','),
                                 ', ', allcols, '))), 0) ',
                                 'INTO @crashpoint_cnt, @crashpoint_sum ',
                                 'FROM `cp`.`', tbl,
                                 '` FORCE INDEX (`', idx, '`)');
    ELSE
      SET @crashpoint_q = CONCAT('SELECT COUNT(*), ',
                                 'IFNULL(SUM(CRC32(CONCAT_WS(', QUOTE(','),
                                 ', ', cols, ', `', pkcol, '`))), 0) ',
                                 'INTO @crashpoint_cnt, @crashpoint_sum ',
                                 'FROM `cp`.`', tbl,
                                 '` FORCE INDEX (`', idx, '`)');
    END IF;
    CALL crashpoint_run(@crashpoint_q);
    SET @crashpoint_state = CONCAT(@crashpoint_state, '|', tbl, '.', idx,
                                   '=', @crashpoint_cnt, '/',
                                   @crashpoint_sum);
    # Part 2, secondary indexes only: every column of every row, read in
    # the order of the index. The ORDER BY inside the derived table makes
    # the optimizer scan the forced index (without it, a non-covering
    # FORCE INDEX scan with no condition is a table scan, which would just
    # repeat the primary key checksum); the entry is followed by the
    # primary key lookup.
    IF idx <> 'PRIMARY' THEN
      SET @crashpoint_q = CONCAT('SELECT COUNT(*), ',
                                 'IFNULL(SUM(CRC32(CONCAT_WS(', QUOTE(','),
                                 ', ', allcols, '))), 0) ',
                                 'INTO @crashpoint_cnt, @crashpoint_sum ',
                                 'FROM (SELECT * FROM `cp`.`', tbl,
                                 '` FORCE INDEX (`', idx, '`) ORDER BY ',
                                 ordcols, ' LIMIT 1000000) AS d');
      CALL crashpoint_run(@crashpoint_q);
      SET @crashpoint_state = CONCAT(@crashpoint_state, '|', tbl, '.', idx,
                                     '/rows=', @crashpoint_cnt, '/',
                                     @crashpoint_sum);
    END IF;
  END LOOP;
  CLOSE cur;
  SET @crashpoint_state = CONCAT(@crashpoint_state, ' ERR_PREP=',
                                 IFNULL(@crashpoint_err_prep, 'none'),
                                 ' ERR_EXEC=', IFNULL(@crashpoint_err_exec, 'none'),
                                 ' ERR_DEAL=',
                                 IFNULL(@crashpoint_err_dealloc, 'none'));
  SET @crashpoint_state = REPLACE(@crashpoint_state, ' ', '_');
END|
delimiter ;|
--enable_warnings
--enable_query_log
