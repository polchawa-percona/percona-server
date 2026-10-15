#
# Scratch (crash campaign): table shapes for crash-at-call enumeration over
# the DDL workloads of errinj_ddl_workloads.inc. ce_reset(shape) recreates
# test.t2 (columns pk, a, b, d and index kb) in the given shape with 30 rows.
#
--source suite/rocksdb/include/errinj_ddl_setup.inc
--disable_query_log
delimiter |;
CREATE PROCEDURE ce_reset(shape VARCHAR(16))
BEGIN
  DECLARE i INT DEFAULT 1;
  DROP TABLE IF EXISTS t2, t2_renamed;
  CASE shape
  WHEN 'pk' THEN
    CREATE TABLE t2 (pk INT PRIMARY KEY, a INT NOT NULL, b VARCHAR(32) NOT NULL,
      d INT NOT NULL, KEY kb (b)) ENGINE=ROCKSDB;
  WHEN 'hidden' THEN
    CREATE TABLE t2 (pk INT NOT NULL, a INT NOT NULL, b VARCHAR(32) NOT NULL,
      d INT NOT NULL, KEY kb (b)) ENGINE=ROCKSDB;
  WHEN 'ttl' THEN
    CREATE TABLE t2 (pk INT PRIMARY KEY, a INT NOT NULL, b VARCHAR(32) NOT NULL,
      d INT NOT NULL, KEY kb (b)) ENGINE=ROCKSDB
      COMMENT='ttl_duration=100000;';
  WHEN 'coll' THEN
    CREATE TABLE t2 (pk INT PRIMARY KEY, a INT NOT NULL,
      b VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
      d INT NOT NULL, KEY kb (b)) ENGINE=ROCKSDB;
  WHEN 'revcf' THEN
    CREATE TABLE t2 (pk INT, a INT NOT NULL, b VARCHAR(32) NOT NULL,
      d INT NOT NULL, PRIMARY KEY (pk) COMMENT 'cfname=rev:ce_rev_pk',
      KEY kb (b) COMMENT 'cfname=rev:ce_rev_sk') ENGINE=ROCKSDB;
  WHEN 'part' THEN
    CREATE TABLE t2 (pk INT PRIMARY KEY, a INT NOT NULL, b VARCHAR(32) NOT NULL,
      d INT NOT NULL, KEY kb (b)) ENGINE=ROCKSDB
      PARTITION BY HASH (pk) PARTITIONS 3;
  WHEN 'partial' THEN
    CREATE TABLE t2 (g INT NOT NULL, pk INT NOT NULL, a INT NOT NULL,
      b VARCHAR(32) NOT NULL, d INT NOT NULL, PRIMARY KEY (g, pk),
      KEY kb (g, b) COMMENT 'partial_group_keyparts=1;partial_group_threshold=5')
      ENGINE=ROCKSDB;
  WHEN 'instant' THEN
    CREATE TABLE t2 (pk INT PRIMARY KEY, a INT NOT NULL, b VARCHAR(32) NOT NULL,
      d INT NOT NULL, KEY kb (b)) ENGINE=ROCKSDB;
  WHEN 'blob' THEN
    CREATE TABLE t2 (pk INT PRIMARY KEY, a INT NOT NULL, b TEXT NOT NULL,
      d INT NOT NULL, j JSON, KEY kb (b(8))) ENGINE=ROCKSDB;
  END CASE;
  WHILE i <= 30 DO
    IF shape = 'partial' THEN
      INSERT INTO t2 (g, pk, a, b, d) VALUES (i MOD 3, i, i, CONCAT('b', i MOD 7), 300 - i);
    ELSEIF shape = 'blob' THEN
      INSERT INTO t2 VALUES (i, i, CONCAT('b', i MOD 7, REPEAT('x', 20)), 300 - i,
        JSON_OBJECT('i', i));
    ELSEIF shape = 'instant' THEN
      INSERT INTO t2 VALUES (i, i, CONCAT('b', i MOD 7), 300 - i);
    ELSE
      INSERT INTO t2 (pk, a, b, d) VALUES (i, i, CONCAT('b', i MOD 7), 300 - i);
    END IF;
    SET i = i + 1;
  END WHILE;
  IF shape = 'instant' THEN
    ALTER TABLE t2 ADD COLUMN ic INT NOT NULL DEFAULT 7, ALGORITHM=INSTANT;
    UPDATE t2 SET ic = 8 WHERE pk <= 10;
  END IF;
END|
CREATE PROCEDURE ce_reset_durable(shape VARCHAR(16))
BEGIN
  DECLARE m INT DEFAULT @@global.rocksdb_flush_log_at_trx_commit;
  SET GLOBAL rocksdb_flush_log_at_trx_commit = 1;
  CALL ce_reset(shape);
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  SET GLOBAL rocksdb_flush_log_at_trx_commit = m;
END|
delimiter ;|
--enable_query_log
