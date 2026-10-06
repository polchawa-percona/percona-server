-- Run on Percona Server 8.0 with MyRocks loaded, binary log enabled.
-- Creates the schema "upg" used by rocksdb.upgrade_from_80; the datadir is
-- then shut down cleanly and zipped as data80_rocks_upgrade.zip.
-- After this script, upgrade_80_checks.sql is run on 8.0 to record the
-- expected values that the 8.4 test compares with.

CREATE DATABASE upg;
USE upg;

-- Column types and collations, with secondary indexes over each kind of
-- key part (binary, _bin, non-binary collations that need unpack info,
-- prefix keys, unique keys with NULLs).
CREATE TABLE t_types (
  id INT NOT NULL,
  ti TINYINT, tiu TINYINT UNSIGNED, si SMALLINT, mi MEDIUMINT,
  bi BIGINT, biu BIGINT UNSIGNED,
  de DECIMAL(20,6), fl FLOAT, db DOUBLE, bt BIT(13),
  d DATE, t TIME(3), dt DATETIME(6), ts TIMESTAMP(2) NULL, y YEAR,
  c_bin CHAR(12) CHARACTER SET latin1 COLLATE latin1_bin,
  vc_lat VARCHAR(40) CHARACTER SET latin1 COLLATE latin1_swedish_ci,
  vc_u8bin VARCHAR(40) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
  vc_u8ai VARCHAR(40) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci,
  c_u8gen CHAR(10) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci,
  vc_u8 VARCHAR(40) CHARACTER SET utf8mb3 COLLATE utf8mb3_unicode_ci,
  bn BINARY(8), vb VARBINARY(32),
  bl BLOB, tx TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci,
  js JSON, en ENUM('a','bb','ccc'), st SET('x','y','z'),
  uq VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci,
  PRIMARY KEY (id),
  KEY k_int (ti, si, mi, bi),
  KEY k_num (de, db),
  KEY k_time (d, t, dt, ts, y),
  KEY k_cbin (c_bin),
  KEY k_lat (vc_lat),
  KEY k_u8bin (vc_u8bin),
  KEY k_u8ai (vc_u8ai),
  KEY k_u8gen (c_u8gen),
  KEY k_u8 (vc_u8),
  KEY k_bin (bn, vb),
  KEY k_prefix (bl(6), tx(5)),
  KEY k_enum (en, st),
  UNIQUE KEY uk (uq)
) ENGINE=ROCKSDB;

-- Composite primary key on non-binary collations (PK unpack info).
CREATE TABLE t_pk_coll (
  a VARCHAR(30) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  b CHAR(5) CHARACTER SET latin1 COLLATE latin1_swedish_ci NOT NULL,
  c INT,
  PRIMARY KEY (a, b),
  KEY k_c (c)
) ENGINE=ROCKSDB;

-- Reverse and custom column families.
CREATE TABLE t_cf (
  id INT NOT NULL,
  a INT, b VARCHAR(20),
  PRIMARY KEY (id) COMMENT 'cfname=rev:upg_rev_cf',
  KEY ka (a) COMMENT 'cfname=upg_custom_cf',
  KEY kb (b) COMMENT 'rev:upg_rev_sk_cf'
) ENGINE=ROCKSDB;

-- Partitioned tables: per-partition column families, hash partitions and
-- a partition-specific TTL.
CREATE TABLE t_part_range (
  id INT NOT NULL, a INT, b VARCHAR(20),
  PRIMARY KEY (id) COMMENT 'p0_cfname=upg_p0;p1_cfname=rev:upg_p1;p2_cfname=upg_p2',
  KEY ka (a)
) ENGINE=ROCKSDB
PARTITION BY RANGE (id) (
  PARTITION p0 VALUES LESS THAN (100),
  PARTITION p1 VALUES LESS THAN (200),
  PARTITION p2 VALUES LESS THAN MAXVALUE);

CREATE TABLE t_part_hash (
  id INT NOT NULL, a INT, b VARCHAR(20),
  PRIMARY KEY (id), KEY kb (b)
) ENGINE=ROCKSDB PARTITION BY HASH (id) PARTITIONS 4;

CREATE TABLE t_part_list (
  id INT NOT NULL, cat INT NOT NULL, v INT,
  PRIMARY KEY (id, cat), KEY kv (v)
) ENGINE=ROCKSDB
PARTITION BY LIST (cat) (
  PARTITION pa VALUES IN (1,2),
  PARTITION pb VALUES IN (3,4));

-- TTL: a table duration of 5 minutes (long enough not to expire while the
-- datadir is generated, long expired when the 8.4 test runs), a duration
-- that never expires, a TTL column with timestamps that expire 6 minutes after
-- generation or never, and a TTL on one partition only.
CREATE TABLE t_ttl_short (id INT PRIMARY KEY, a INT, KEY ka (a))
  ENGINE=ROCKSDB COMMENT 'ttl_duration=300;';
CREATE TABLE t_ttl_long (id INT PRIMARY KEY, a INT, KEY ka (a))
  ENGINE=ROCKSDB COMMENT 'ttl_duration=3000000000;';
CREATE TABLE t_ttl_col (id INT PRIMARY KEY, a INT,
  ts BIGINT UNSIGNED NOT NULL, KEY ka (a))
  ENGINE=ROCKSDB COMMENT 'ttl_duration=60;ttl_col=ts;';
CREATE TABLE t_ttl_part (id INT NOT NULL, a INT, PRIMARY KEY (id))
  ENGINE=ROCKSDB COMMENT 'custom_p0_ttl_duration=300;'
  PARTITION BY RANGE (id) (
    PARTITION custom_p0 VALUES LESS THAN (100),
    PARTITION custom_p1 VALUES LESS THAN MAXVALUE);

-- Partial secondary index.
CREATE TABLE t_partial (
  id1 INT NOT NULL, id2 INT NOT NULL, v INT,
  PRIMARY KEY (id1, id2),
  KEY kp (id1, v) COMMENT 'partial_group_keyparts=1;partial_group_threshold=5'
) ENGINE=ROCKSDB;

-- Hidden primary key.
CREATE TABLE t_nopk (a INT, b VARCHAR(10), KEY ka (a)) ENGINE=ROCKSDB;

-- Auto-increment with a gap and a reserved range.
CREATE TABLE t_autoinc (id BIGINT NOT NULL AUTO_INCREMENT, a INT,
  PRIMARY KEY (id), KEY ka (a)) ENGINE=ROCKSDB AUTO_INCREMENT=1;
-- Auto-increment on a secondary key column.
CREATE TABLE t_autoinc_sk (pk INT PRIMARY KEY, id INT NOT NULL AUTO_INCREMENT,
  KEY kid (id)) ENGINE=ROCKSDB;

-- Generated columns, stored and virtual, both indexed.
CREATE TABLE t_gen (
  id INT PRIMARY KEY, a INT, b VARCHAR(20),
  gs INT AS (a * 2) STORED,
  gv VARCHAR(25) AS (CONCAT(b, '-v')) VIRTUAL,
  KEY kgs (gs), KEY kgv (gv)
) ENGINE=ROCKSDB;

-- Instant DDL: columns appended instantly, a default changed instantly and
-- a table comment changed instantly, with rows written before and after.
CREATE TABLE t_instant (id INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=ROCKSDB;

-- Trigger and view on MyRocks tables, and an InnoDB table, to check the
-- data dictionary upgrade of mixed objects.
CREATE TABLE t_log (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, msg VARCHAR(40))
  ENGINE=ROCKSDB;
CREATE TABLE t_inno (id INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=InnoDB;

-- Data.
DELIMITER //
CREATE PROCEDURE fill()
BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < 300 DO
    INSERT INTO t_types VALUES (i,
      i % 128 - 64, i % 256, i * 7 - 1000, i * 113, i * 1000003 - 5, i * 99991,
      i * 1.25 - 3.5, i / 7, i / 3.0, i % 8192,
      DATE_ADD('2001-02-03', INTERVAL i DAY),
      SEC_TO_TIME(i * 37.125),
      TIMESTAMPADD(MICROSECOND, i * 1234567, '2010-05-06 07:08:09.123456'),
      IF(i % 10 = 0, NULL, TIMESTAMPADD(SECOND, i * 3600, '2015-01-01 00:00:00.25')),
      1901 + i % 200,
      CONCAT('cb', i % 37),
      CONCAT(ELT(i % 4 + 1, 'Ärger', 'arger', 'ÄRGER', 'zebra'), ' ', i % 11),
      CONCAT(ELT(i % 3 + 1, 'ąę', 'Zażółć', 'abc'), i),
      CONCAT(ELT(i % 5 + 1, 'Straße', 'strasse', 'STRASSE', 'café', 'cafe'), ' ', i % 13),
      ELT(i % 3 + 1, 'Bar', 'bar  ', 'BAR'),
      CONCAT(ELT(i % 2 + 1, 'Łódź', 'lodz'), i % 17),
      UNHEX(LPAD(HEX(i), 16, '0')), UNHEX(REPEAT(HEX(i % 251), 1 + i % 9)),
      REPEAT(CHAR(65 + i % 26), 1 + i % 50),
      CONCAT('tekst ', REPEAT('ż', i % 7), i),
      JSON_OBJECT('i', i, 'arr', JSON_ARRAY(i, i + 1), 's', CONCAT('v', i)),
      ELT(i % 3 + 1, 'a', 'bb', 'ccc'),
      ELT(i % 4 + 1, 'x', 'x,y', 'y,z', ''),
      IF(i % 17 = 0, NULL, CONCAT('uq', i)));
    INSERT INTO t_pk_coll VALUES (CONCAT(ELT(i % 3 + 1, 'Ala', 'Ola', 'Ela'), i), ELT(i % 2 + 1, 'k', 'm'), i % 23);
    INSERT INTO t_cf VALUES (i, i % 29, CONCAT('b', i % 41));
    INSERT INTO t_part_range VALUES (i, i % 31, CONCAT('r', i));
    INSERT INTO t_part_hash VALUES (i, i % 19, CONCAT('h', i % 53));
    INSERT INTO t_part_list VALUES (i, 1 + i % 4, i % 7);
    INSERT INTO t_ttl_short VALUES (i, i % 5);
    INSERT INTO t_ttl_long VALUES (i, i % 5);
    INSERT INTO t_ttl_col VALUES (i, i % 5, IF(i % 2 = 0, UNIX_TIMESTAMP() + 300, 4102444800));
    INSERT INTO t_ttl_part VALUES (i, i % 5);
    INSERT INTO t_partial VALUES (i % 7, i, i % 13);
    INSERT INTO t_nopk VALUES (i % 43, CONCAT('n', i));
    INSERT INTO t_gen (id, a, b) VALUES (i, i % 37, CONCAT('g', i % 41));
    INSERT INTO t_instant VALUES (i, i % 11);
    INSERT INTO t_inno VALUES (i, i % 9);
    SET i = i + 1;
  END WHILE;
END//
CREATE TRIGGER t_cf_ai AFTER INSERT ON t_cf FOR EACH ROW
  INSERT INTO t_log (msg) VALUES (CONCAT('cf ', NEW.id))//
DELIMITER ;

CALL fill();
DROP PROCEDURE fill;

CREATE VIEW v_join AS
  SELECT c.id, c.a, i.a AS ia FROM t_cf c JOIN t_inno i ON i.id = c.id;

-- Auto-increment: values 1..200, then a gap up to 1000.
INSERT INTO t_autoinc (a) SELECT id % 10 FROM t_cf WHERE id < 200;
INSERT INTO t_autoinc VALUES (1000, 0);
INSERT INTO t_autoinc_sk (pk) SELECT id FROM t_cf WHERE id < 50;

-- Instant DDL.
SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = ON;
SET GLOBAL rocksdb_enable_instant_ddl_for_column_default_changes = ON;
SET GLOBAL rocksdb_enable_instant_ddl_for_table_comment_changes = ON;
ALTER TABLE t_instant ADD COLUMN b INT DEFAULT 7, ALGORITHM=INSTANT;
ALTER TABLE t_instant ADD COLUMN c VARCHAR(20) NOT NULL DEFAULT 'instant_c',
  ADD COLUMN d DATETIME DEFAULT '2020-01-02 03:04:05', ALGORITHM=INSTANT;
INSERT INTO t_instant VALUES (1000, 1, 2, 'after', '2021-01-01 00:00:00');
UPDATE t_instant SET b = 70 WHERE id BETWEEN 10 AND 19;
ALTER TABLE t_instant ALTER COLUMN b SET DEFAULT 8, ALGORITHM=INSTANT;
ALTER TABLE t_instant COMMENT 'instant comment', ALGORITHM=INSTANT;
INSERT INTO t_instant (id, a) VALUES (1001, 3);
SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = DEFAULT;
SET GLOBAL rocksdb_enable_instant_ddl_for_column_default_changes = DEFAULT;
SET GLOBAL rocksdb_enable_instant_ddl_for_table_comment_changes = DEFAULT;

-- Some deletes and updates so that SST files hold tombstones and
-- overwritten versions.
DELETE FROM t_cf WHERE id % 10 = 3;
UPDATE t_part_range SET a = a + 1000 WHERE id % 7 = 0;
DELETE FROM t_part_hash WHERE id BETWEEN 50 AND 60;
UPDATE t_types SET vc_u8ai = CONCAT(vc_u8ai, '!') WHERE id % 9 = 0;

-- Part of the data is flushed to SST files; the rest stays in the memtable
-- until the clean shutdown flushes it.
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
INSERT INTO t_cf VALUES (5000, 1, 'late'), (5001, 2, 'late');
