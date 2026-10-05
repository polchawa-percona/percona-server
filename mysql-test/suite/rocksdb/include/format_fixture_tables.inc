#
# Creates the tables stored in std_data/data84_rocks_legacy_formats.zip.
#
# Each group of tables is created while a debug keyword forces MyRocks to
# use an older key/value format version for the new indexes (see
# ha_rocksdb::create_key_def). Only CREATE TABLE runs under the keyword; the
# rows are written afterwards, in whatever format the index was created with.
# Requires a debug build. Used only by format_fixture_gen.test.
#
#   MYROCKS_LEGACY_VARBINARY_FORMAT          PK v11, SK v11 (old binary
#                                            variable-length encoding)
#   MYROCKS_NO_COVERED_BITMAP_FORMAT         PK latest, SK v12 (no covered
#                                            bitmap in unpack_info)
#   MYROCKS_NO_LEAD_BYTE_VARCHAR_SPACE_PADDED PK v13, SK v13 (space-padded
#                                            VARCHAR without the lead byte)
#
# PK v14 / SK v15 (before the NO_PAD collation fix) cannot be forced with a
# debug keyword; std_data/data80_rocks_index_ver_bump.zip covers them.
#

# INSERT ... SELECT below would hit the gap-lock check at REPEATABLE READ.
SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED;
CREATE DATABASE fmt;
USE fmt;

--echo # Group v11: MYROCKS_LEGACY_VARBINARY_FORMAT
SET SESSION debug = '+d,MYROCKS_LEGACY_VARBINARY_FORMAT';
CREATE TABLE v11_vb (
  vb VARBINARY(64) PRIMARY KEY,
  n INT NOT NULL,
  KEY k_n (n)
) ENGINE=ROCKSDB;
CREATE TABLE v11_vb_sk (
  id INT PRIMARY KEY,
  vb VARBINARY(64),
  KEY k_vb (vb)
) ENGINE=ROCKSDB;
CREATE TABLE v11_vc_bin (
  vc VARCHAR(64) COLLATE 'binary' PRIMARY KEY
) ENGINE=ROCKSDB;
CREATE TABLE v11_mixed (
  id INT PRIMARY KEY,
  d DECIMAL(10,3),
  vc VARCHAR(32) CHARACTER SET latin1 COLLATE latin1_swedish_ci,
  vb VARBINARY(32),
  t TEXT CHARACTER SET latin1,
  KEY k_d (d),
  KEY k_vc (vc),
  KEY k_vb (vb),
  KEY k_t (t(10))
) ENGINE=ROCKSDB;
SET SESSION debug = '-d,MYROCKS_LEGACY_VARBINARY_FORMAT';

INSERT INTO v11_vb VALUES (0x00, 1), (0x0000, 2), (0x000000, 3), (0x00000000, 4),
  (0x0000000000, 5), (0x000000000000, 6), (0x00000000000000, 7),
  (0x0000000000000000, 8), (0x000000000000000000, 9),
  (0x00000000000000000000, 10),
  (0x00000000000000000000000000000000000000000000000000000000000000, 31),
  (0x0000000000000000000000000000000000000000000000000000000000000000, 32),
  (0x000000000000000000000000000000000000000000000000000000000000000000, 33),
  (0x41, 101), (0x4142434445464748, 108), (0x414243444546474849, 109), (0xFF, 255);
INSERT INTO v11_vb_sk SELECT n, vb FROM v11_vb;
INSERT INTO v11_vb_sk VALUES (1000, NULL), (1001, '');
INSERT INTO v11_vc_bin VALUES ('a'), ('aa'), ('aaa'), ('aaaa'), ('aaaaa'),
  ('aaaaaa'), ('aaaaaaa'), ('aaaaaaaa'), ('aaaaaaaaa'), ('aaaaaaaaaa'),
  ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'), ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'),
  ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'), ('b'), ('');
INSERT INTO v11_mixed VALUES
  (1, 0.000, '', '', ''),
  (2, -1.500, 'abc', 0x616263, 'short text'),
  (3, 12345.678, 'ABC', 0x41424344454647, 'text that is longer than the prefix'),
  (4, -9999999.999, 'abc   ', 0x4142434445464748, 'TEXT THAT IS LONGER THAN THE PREFIX'),
  (5, 9999999.999, 'Åsa', 0x414243444546474849, 'åäö'),
  (6, NULL, NULL, NULL, NULL),
  (7, 0.001, 'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz', REPEAT(0x7a, 32), REPEAT('z', 300));

--echo # Group v12: MYROCKS_NO_COVERED_BITMAP_FORMAT
SET SESSION debug = '+d,MYROCKS_NO_COVERED_BITMAP_FORMAT';
CREATE TABLE v12_cov (
  id INT,
  fake_id INT,
  bigfield VARCHAR(4096),
  PRIMARY KEY (id),
  KEY bf (bigfield(32)),
  KEY fid (fake_id, bigfield(32))
) ENGINE=ROCKSDB;
SET SESSION debug = '-d,MYROCKS_NO_COVERED_BITMAP_FORMAT';

INSERT INTO v12_cov VALUES (1, 1001, REPEAT('a', 1)),
                           (8, 1008, REPEAT('b', 8)),
                           (24, 1024, REPEAT('c', 24)),
                           (31, 1031, REPEAT('d', 31)),
                           (32, 1032, REPEAT('x', 32)),
                           (33, 1033, REPEAT('y', 33)),
                           (128, 1128, REPEAT('z', 128));

--echo # Group v13: MYROCKS_NO_LEAD_BYTE_VARCHAR_SPACE_PADDED
SET SESSION debug = '+d,MYROCKS_NO_LEAD_BYTE_VARCHAR_SPACE_PADDED';
CREATE TABLE v13_pk_latin1_bin (
  vc VARCHAR(64) PRIMARY KEY
) ENGINE=ROCKSDB CHARACTER SET latin1 COLLATE latin1_bin;
CREATE TABLE v13_pk_composite (
  a INT,
  vc VARCHAR(64),
  PRIMARY KEY (a, vc)
) ENGINE=ROCKSDB CHARACTER SET latin1 COLLATE latin1_bin;
CREATE TABLE v13_sk (
  a INT PRIMARY KEY,
  vc VARCHAR(64),
  KEY k_vc (vc)
) ENGINE=ROCKSDB CHARACTER SET latin1 COLLATE latin1_bin;
CREATE TABLE v13_pk_utf8mb4_bin (
  vc VARCHAR(64) PRIMARY KEY
) ENGINE=ROCKSDB CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;
CREATE TABLE v13_ci (
  id INT PRIMARY KEY,
  vc VARCHAR(64),
  c CHAR(16),
  KEY k_vc (vc),
  UNIQUE KEY uk_c (c)
) ENGINE=ROCKSDB CHARACTER SET latin1 COLLATE latin1_swedish_ci;
CREATE TABLE v13_ttl (
  id INT PRIMARY KEY,
  vc VARCHAR(32),
  ts BIGINT UNSIGNED NOT NULL,
  KEY k_vc (vc)
) ENGINE=ROCKSDB CHARACTER SET latin1 COLLATE latin1_bin
  COMMENT 'ttl_duration=31536000;ttl_col=ts';
SET SESSION debug = '-d,MYROCKS_NO_LEAD_BYTE_VARCHAR_SPACE_PADDED';

INSERT INTO v13_pk_latin1_bin VALUES (''), (0x0000), (0x0000000000000000),
  (0x000000000000000000), ('ABC'), ('abc'), ('abc  x'),
  (REPEAT('q', 8)), (REPEAT('q', 9)), (REPEAT('q', 17)), (REPEAT('q', 64));
INSERT INTO v13_pk_composite VALUES (1, ''), (2, '  '), (3, '        '),
  (4, '          '), (5, 'abc'), (6, 'abc     '), (7, 'abc       '),
  (8, REPEAT('w', 33));
INSERT INTO v13_sk VALUES (1, ''), (2, '  '), (5, 'abc'), (6, 'abc     '),
  (7, 'abc       '), (3, '        '), (4, '          '), (8, NULL),
  (9, REPEAT('w', 33));
INSERT INTO v13_pk_utf8mb4_bin VALUES (''), (0x0000), (0x0000000000000000),
  (0x000000000000000000), ('ABC'), ('żółw'), ('😀'), (REPEAT('ę', 20));
INSERT INTO v13_ci VALUES (1, 'abc', 'abc'), (2, 'ABC', 'ABD'),
  (3, 'abc   ', 'aBe'), (4, 'Åsa', 'åsa'), (5, '', ''), (6, NULL, NULL),
  (7, 'zzz', NULL), (8, REPEAT('m', 40), 'mmmmmmmmmmmmmmmm');
INSERT INTO v13_ttl VALUES (1, 'alpha', 4000000000), (2, 'beta  ', 4000000000),
  (3, '', 4000000000), (4, REPEAT('g', 32), 4000000000);
USE test;
