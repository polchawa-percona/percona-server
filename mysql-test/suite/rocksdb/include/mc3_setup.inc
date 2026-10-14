# Setup for the manual compaction vs table DDL tests (mc3_*).
# Creates schemas mc3 and mc3b, the InnoDB bookkeeping tables and the
# checksum procedures. Tables are created by mc3_create.inc.

--disable_query_log
CREATE DATABASE mc3;
CREATE DATABASE mc3b;
# Per-index row count and checksum, by label.
CREATE TABLE mc3.chk (label VARCHAR(32), tbl VARCHAR(64), idx VARCHAR(64),
  cnt BIGINT, crc BIGINT, PRIMARY KEY (label, tbl, idx)) ENGINE=InnoDB;
# Index ids of dropped or replaced indexes, to check they are reclaimed.
CREATE TABLE mc3.dropped (cf INT, idx INT, PRIMARY KEY (cf, idx))
  ENGINE=InnoDB;

delimiter |;
# Records COUNT(*) and an order-independent checksum of the rows read
# through each index in the comma-separated list idxs. An empty idxs entry
# reads the table without FORCE INDEX (for hidden primary keys).
CREATE PROCEDURE mc3.snap(lbl VARCHAR(32), tbl VARCHAR(64), cols TEXT,
                          idxs TEXT)
BEGIN
  DECLARE rest TEXT DEFAULT idxs;
  DECLARE idx VARCHAR(64);
  DECLARE force_clause TEXT;
  REPEAT
    SET idx = SUBSTRING_INDEX(rest, ',', 1);
    SET rest = IF(LOCATE(',', rest) > 0,
                  SUBSTRING(rest, LOCATE(',', rest) + 1), NULL);
    SET force_clause = IF(idx = '', '', CONCAT(' FORCE INDEX (`', idx, '`)'));
    SET @mc3_sql = CONCAT('INSERT INTO mc3.chk SELECT ''', lbl, ''', ''',
                          tbl, ''', ''', IF(idx = '', '-', idx),
                          ''', COUNT(*), COALESCE(BIT_XOR(CRC32(CONCAT_WS(''#'', ',
                          cols, '))), 0) FROM ', tbl, force_clause);
    PREPARE s FROM @mc3_sql;
    EXECUTE s;
    DEALLOCATE PREPARE s;
  UNTIL rest IS NULL END REPEAT;
END|

# Records the index ids of a table, to check later that they are reclaimed.
CREATE PROCEDURE mc3.mark_dropped(sch VARCHAR(64), tbl VARCHAR(64))
BEGIN
  INSERT IGNORE INTO mc3.dropped
    SELECT column_family, index_number FROM information_schema.rocksdb_ddl
    WHERE table_schema = sch AND table_name = tbl;
END|
delimiter ;|
--enable_query_log
