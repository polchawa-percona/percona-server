-- Run on Percona Server 8.0 with MyRocks loaded. Prepares the schema
-- "upgc" for data80_rocks_upgrade_crash.zip. gen_upgrade_datadirs.sh then
-- records the expected values, starts an unfinished bulk load, drops t_drop,
-- starts a COPY ALTER of t_alter and kills the server with SIGKILL.

CREATE DATABASE upgc;
USE upgc;

CREATE TABLE seq1000 (seq INT PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO seq1000 WITH RECURSIVE r(n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM r WHERE n < 999)
  SELECT n FROM r;

-- Rows of t_wal written before the last flush are in SST files; the rows
-- written, updated and deleted afterwards are only in the WAL.
CREATE TABLE t_wal (id INT PRIMARY KEY, a INT, b VARCHAR(30)
  CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci,
  KEY ka (a), KEY kb (b) COMMENT 'rev:upgc_rev') ENGINE=ROCKSDB;
INSERT INTO t_wal SELECT seq, seq % 17, CONCAT('Wal ', seq % 101) FROM seq1000 WHERE seq < 500;
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
INSERT INTO t_wal SELECT seq, seq % 17, CONCAT('wal ', seq % 101) FROM seq1000 WHERE seq >= 500;
UPDATE t_wal SET a = a + 100, b = CONCAT(b, ' u') WHERE id % 5 = 0;
DELETE FROM t_wal WHERE id % 7 = 0;

CREATE TABLE t_wal_part (id INT NOT NULL, a INT, PRIMARY KEY (id), KEY ka (a))
  ENGINE=ROCKSDB PARTITION BY HASH (id) PARTITIONS 3;
INSERT INTO t_wal_part SELECT seq, seq % 9 FROM seq1000;

-- An unfinished bulk load adds rows 100..599 to t_bulk, fewer than
-- rocksdb_bulk_load_size, so none of them is ingested; only the rows below
-- are committed. The table has no secondary key: a crash during a bulk load
-- into a table with one leaves it inconsistent (PS-11696, tested by
-- rocksdb.crash_bulk_load_sk_consistency).
CREATE TABLE t_bulk (id INT PRIMARY KEY, a INT) ENGINE=ROCKSDB;
INSERT INTO t_bulk SELECT seq, seq FROM seq1000 WHERE seq < 10;

-- t_drop is dropped before the kill; t_drop_keep shares its column family
-- and must keep all of its rows.
CREATE TABLE t_drop (id INT PRIMARY KEY, a INT,
  KEY ka (a) COMMENT 'cfname=upgc_drop_cf') ENGINE=ROCKSDB;
INSERT INTO t_drop SELECT seq, seq FROM seq1000;
INSERT INTO t_drop SELECT seq + 1000, seq FROM seq1000;
CREATE TABLE t_drop_keep (id INT PRIMARY KEY, a INT,
  KEY ka (a) COMMENT 'cfname=upgc_drop_cf') ENGINE=ROCKSDB;
INSERT INTO t_drop_keep SELECT seq, seq % 3 FROM seq1000;

-- t_alter is rebuilt by a COPY ALTER that is killed half-way.
CREATE TABLE t_alter (id INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=ROCKSDB;
INSERT INTO t_alter SELECT seq, seq % 13 FROM seq1000;
INSERT INTO t_alter SELECT seq + 1000, seq % 13 FROM seq1000;
INSERT INTO t_alter SELECT seq + 2000, seq % 13 FROM seq1000;
