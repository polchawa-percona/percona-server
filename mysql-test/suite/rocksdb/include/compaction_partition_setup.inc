# Creates the partitioned MyRocks tables used by the compaction_partition
# tests and an InnoDB mirror <table>_ref of each, with the same partitioning
# and the same rows. Every change made to a MyRocks table is also made to its
# mirror; compaction_partition_check.inc compares them partition by partition.
#
#   tr  RANGE (id), p0..p3; PRIMARY KEY (p0 in mc4_p0, p1 in rev:mc4_p1,
#       p3 in mc4_shared), UNIQUE KEY uk (u, id) (p0 in mc4_p0_sk, p3 in
#       mc4_shared), prefix KEY kc (c(8)); BLOB column.
#   tl  LIST (id DIV 100), p0..p3; PRIMARY KEY (p0 in mc4_l0, p2 in
#       rev:mc4_l2), prefix KEY kc (c(8)); BLOB column.
#   th  HASH (id), 4 partitions, hidden primary key, KEY ku (u), KEY kc (c(8)).
#   tk  KEY (id), 3 partitions, PRIMARY KEY only, all in mc4_shared; BLOB.
--disable_query_log
CREATE TABLE seq (n INT PRIMARY KEY) ENGINE = InnoDB;
INSERT INTO seq WITH RECURSIVE s (n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM s WHERE n < 999) SELECT n FROM s;
let $cp_i = 2;
while ($cp_i)
{
  let $sfx = `SELECT IF($cp_i = 2, '', '_ref')`;
  let $eng = `SELECT IF($cp_i = 2, 'ROCKSDB', 'InnoDB')`;
  eval CREATE TABLE tr$sfx (
    id INT NOT NULL, u INT NOT NULL, c VARCHAR(64), b BLOB,
    PRIMARY KEY (id) COMMENT 'p0_cfname=mc4_p0;p1_cfname=rev:mc4_p1;p3_cfname=mc4_shared',
    UNIQUE KEY uk (u, id) COMMENT 'p0_cfname=mc4_p0_sk;p3_cfname=mc4_shared',
    KEY kc (c(8))
  ) ENGINE = $eng
  PARTITION BY RANGE (id) (
    PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN (200),
    PARTITION p2 VALUES LESS THAN (300), PARTITION p3 VALUES LESS THAN (400));
  eval CREATE TABLE tl$sfx (
    id INT NOT NULL, c VARCHAR(64), b BLOB,
    PRIMARY KEY (id) COMMENT 'p0_cfname=mc4_l0;p2_cfname=rev:mc4_l2',
    KEY kc (c(8))
  ) ENGINE = $eng
  PARTITION BY LIST (id DIV 100) (
    PARTITION p0 VALUES IN (0), PARTITION p1 VALUES IN (1),
    PARTITION p2 VALUES IN (2), PARTITION p3 VALUES IN (3));
  eval CREATE TABLE th$sfx (
    id INT NOT NULL, u INT, c VARCHAR(64), KEY ku (u), KEY kc (c(8))
  ) ENGINE = $eng PARTITION BY HASH (id) PARTITIONS 4;
  eval CREATE TABLE tk$sfx (
    id INT NOT NULL, b BLOB, PRIMARY KEY (id) COMMENT 'cfname=mc4_shared'
  ) ENGINE = $eng PARTITION BY KEY (id) PARTITIONS 3;
  eval INSERT INTO tr$sfx SELECT n, n * 7, CONCAT(REPEAT('x', n % 7), '-', n), REPEAT(CHAR(65 + n % 26), 100 + n % 50) FROM seq WHERE n < 400;
  eval INSERT INTO tl$sfx SELECT n, CONCAT(REPEAT('y', n % 5), '-', n), REPEAT(CHAR(97 + n % 26), 50 + n % 30) FROM seq WHERE n < 400;
  eval INSERT INTO th$sfx SELECT n, n % 37, CONCAT(REPEAT('z', n % 3), '-', n) FROM seq WHERE n < 400;
  eval INSERT INTO tk$sfx SELECT n, REPEAT(CHAR(48 + n % 10), 200 + n % 20) FROM seq WHERE n < 300;
  dec $cp_i;
}
--enable_query_log
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
