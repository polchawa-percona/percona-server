# Tables for the manual-compaction DML tests (mc_*.test). Shapes: a primary
# key that covers all columns (deletes of it are SingleDeletes), a hidden
# primary key, several secondary keys in the default column family (unique,
# non-unique on a collation with unpack info, prefix on TEXT, covering), keys
# in their own and in a reverse column family, and a partial index.
CREATE DATABASE mc;
CREATE TABLE mc.t_pk (id INT PRIMARY KEY) ENGINE=ROCKSDB;
CREATE TABLE mc.t_hpk (
  a INT NOT NULL,
  b VARCHAR(32) COLLATE utf8mb4_0900_ai_ci NOT NULL,
  KEY ka (a), KEY kb (b)) ENGINE=ROCKSDB;
CREATE TABLE mc.t_multi (
  id INT PRIMARY KEY,
  u INT NOT NULL,
  v VARCHAR(64) COLLATE utf8mb4_0900_ai_ci NOT NULL,
  c TEXT NOT NULL,
  UNIQUE KEY ku (u), KEY kv (v), KEY kc (c(8)), KEY kcov (u, v)) ENGINE=ROCKSDB;
CREATE TABLE mc.t_cf (
  id INT NOT NULL,
  a INT NOT NULL,
  b VARCHAR(32) COLLATE utf8mb4_bin NOT NULL,
  PRIMARY KEY (id) COMMENT 'cfname=mc_pk',
  UNIQUE KEY ka (a) COMMENT 'cfname=mc_sk',
  KEY kb (b) COMMENT 'cfname=rev:mc_rev') ENGINE=ROCKSDB;
CREATE TABLE mc.t_part (
  g INT NOT NULL, id INT NOT NULL, v INT NOT NULL,
  PRIMARY KEY (g, id),
  KEY kp (g, v) COMMENT 'partial_group_keyparts=1;partial_group_threshold=3')
  ENGINE=ROCKSDB;

# Tables that share the column families with the mc tables. OPTIMIZE TABLE
# needs a metadata lock that open transactions on the mc tables hold, so the
# tests run it on these: its CompactRange rewrites every SST file that
# overlaps their ranges, and so the mc rows stored in the same files.
CREATE TABLE mc.side_def (id INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=ROCKSDB;
CREATE TABLE mc.side_cf (
  id INT NOT NULL, a INT NOT NULL, b INT NOT NULL,
  PRIMARY KEY (id) COMMENT 'cfname=mc_pk',
  KEY ka (a) COMMENT 'cfname=mc_sk',
  KEY kb (b) COMMENT 'cfname=rev:mc_rev') ENGINE=ROCKSDB;

--disable_query_log
INSERT INTO mc.side_def VALUES (1, 1), (2, 2);
INSERT INTO mc.side_cf VALUES (1, 1, 1), (2, 2, 2);
# 300 rows: mc_dml.inc changes ids up to 10 * round + 4.
INSERT INTO mc.t_pk
  WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 300)
  SELECT n FROM s;
INSERT INTO mc.t_hpk
  WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 200)
  SELECT n % 50, CONCAT('val', n % 30) FROM s;
INSERT INTO mc.t_multi
  WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 200)
  SELECT n, n * 10, CONCAT(IF(n % 2, 'Abc', 'abc'), n % 20),
         CONCAT(REPEAT('t', n % 7), 'text', n) FROM s;
INSERT INTO mc.t_cf
  WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 200)
  SELECT n, n * 10, CONCAT('b', n % 25) FROM s;
# Group 1 has 2 rows (below the threshold, not materialized), the others more.
INSERT INTO mc.t_part
  WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 200)
  SELECT IF(n <= 2, 1, 2 + n % 10), n, n % 13 FROM s;
--enable_query_log
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
