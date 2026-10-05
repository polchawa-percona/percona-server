#
# Runs every partition operation on RANGE, LIST, HASH, KEY and RANGE with HASH
# subpartitions tables of engine $engine, and records the table state after
# each step with partition_ddl_matrix_check.inc.
#
# Parameters:
#   $engine      InnoDB or ROCKSDB
#   $mode        0: skip DROP PARTITION of a partition whose rows another
#                partition would accept (on MyRocks without
#                rocksdb_allow_unsafe_alter the rows are kept, PS-11634; that
#                case has its own disabled tests). 1: include it.
#
--let $run = $engine/$mode
--echo
--echo ######## engine $engine, mode $mode

--echo
--echo #### RANGE
--let $ptype = RANGE
--let $tbl = tr
eval CREATE TABLE tr (a INT NOT NULL, b INT, c VARCHAR(20), PRIMARY KEY (a), KEY kb (b))
  ENGINE=$engine PARTITION BY RANGE (a)
  (PARTITION p0 VALUES LESS THAN (10), PARTITION p1 VALUES LESS THAN (20),
   PARTITION p2 VALUES LESS THAN (30), PARTITION p3 VALUES LESS THAN (40));
INSERT INTO tr WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 39)
  SELECT n, n MOD 7, CONCAT('v', n) FROM s;
--let $step = create
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tr ADD PARTITION (PARTITION p4 VALUES LESS THAN (50));
INSERT INTO tr VALUES (41, 41 MOD 7, 'v41'), (45, 45 MOD 7, 'v45'), (48, 48 MOD 7, 'v48');
--let $step = add
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tr REORGANIZE PARTITION p0, p1 INTO (PARTITION p01 VALUES LESS THAN (20));
--let $step = reorganize_merge
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tr REORGANIZE PARTITION p4 INTO
  (PARTITION p4a VALUES LESS THAN (45), PARTITION p4b VALUES LESS THAN (50));
--let $step = reorganize_split
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tr TRUNCATE PARTITION p2;
--let $step = truncate
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
--echo # Drop the last partition: no remaining partition accepts its rows.
ALTER TABLE tr DROP PARTITION p4b;
--let $step = drop_last
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
if ($mode == 1)
{
  --echo # Drop a middle partition: the next partition would accept its rows.
  ALTER TABLE tr DROP PARTITION p3;
  --let $step = drop_middle
  --source suite/rocksdb/include/partition_ddl_matrix_check.inc
}
ALTER TABLE tr REBUILD PARTITION p01;
--let $step = rebuild
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
--replace_column 1 x
ALTER TABLE tr ANALYZE PARTITION p01;
--replace_column 1 x
ALTER TABLE tr CHECK PARTITION p01, p2;
--replace_column 1 x
ALTER TABLE tr OPTIMIZE PARTITION p01;
--replace_column 1 x
ALTER TABLE tr REPAIR PARTITION p01;
--let $step = maintenance
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tr REORGANIZE PARTITION p4a INTO
  (PARTITION p4a VALUES LESS THAN (45), PARTITION pmax VALUES LESS THAN MAXVALUE);
INSERT INTO tr VALUES (1000, 1000 MOD 7, 'v1000');
--let $step = add_maxvalue
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
--echo # EXCHANGE PARTITION: MyRocks rejects it (ER_PARTITION_MGMT_ON_NONPARTITIONED),
--echo # InnoDB swaps the rows. The exchange is done twice so that both engines
--echo # continue from the same data.
--let $xch_err = 0
if ($engine == ROCKSDB)
{
  --let $xch_err = 1505
}
eval CREATE TABLE tx (a INT NOT NULL, b INT, c VARCHAR(20), PRIMARY KEY (a), KEY kb (b)) ENGINE=$engine;
INSERT INTO tx VALUES (21, 0, 'x21'), (22, 1, 'x22');
--error $xch_err
ALTER TABLE tr EXCHANGE PARTITION p2 WITH TABLE tx;
SELECT * FROM tr PARTITION (p2) ORDER BY a;
SELECT * FROM tx ORDER BY a;
--error $xch_err
ALTER TABLE tr EXCHANGE PARTITION p2 WITH TABLE tx;
DROP TABLE tx;
--let $step = exchange
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tr REMOVE PARTITIONING;
--let $step = remove_partitioning
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
DROP TABLE tr;

--echo
--echo #### LIST
--let $ptype = LIST
--let $tbl = tl
eval CREATE TABLE tl (a INT NOT NULL, b INT, c VARCHAR(20), PRIMARY KEY (a), KEY kb (b))
  ENGINE=$engine PARTITION BY LIST (a DIV 10)
  (PARTITION l0 VALUES IN (0), PARTITION l1 VALUES IN (1),
   PARTITION l2 VALUES IN (2), PARTITION l3 VALUES IN (3));
INSERT INTO tl WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 39)
  SELECT n, n MOD 7, CONCAT('v', n) FROM s;
--let $step = create
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tl ADD PARTITION (PARTITION l4 VALUES IN (4, 5));
INSERT INTO tl VALUES (41, 41 MOD 7, 'v41'), (52, 52 MOD 7, 'v52');
--let $step = add
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tl REORGANIZE PARTITION l0, l1 INTO (PARTITION l01 VALUES IN (0, 1));
--let $step = reorganize_merge
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tl REORGANIZE PARTITION l4 INTO
  (PARTITION l4 VALUES IN (4), PARTITION l5 VALUES IN (5));
--let $step = reorganize_split
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tl TRUNCATE PARTITION l2;
--let $step = truncate
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
--echo # Drop a LIST partition: no remaining partition accepts its rows.
ALTER TABLE tl DROP PARTITION l3;
--let $step = drop
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tl REBUILD PARTITION l01, l5;
--replace_column 1 x
ALTER TABLE tl ANALYZE PARTITION ALL;
--replace_column 1 x
ALTER TABLE tl CHECK PARTITION ALL;
--let $step = maintenance
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
DROP TABLE tl;

--echo
--echo #### HASH
--let $ptype = HASH
--let $tbl = th
eval CREATE TABLE th (a INT NOT NULL, b INT, c VARCHAR(20), PRIMARY KEY (a), KEY kb (b))
  ENGINE=$engine PARTITION BY HASH (a) PARTITIONS 4;
INSERT INTO th WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 39)
  SELECT n, n MOD 7, CONCAT('v', n) FROM s;
--let $step = create
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE th ADD PARTITION PARTITIONS 2;
--let $step = add
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE th COALESCE PARTITION 3;
--let $step = coalesce
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE th TRUNCATE PARTITION p1;
--let $step = truncate
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE th REBUILD PARTITION p0;
--replace_column 1 x
ALTER TABLE th OPTIMIZE PARTITION p0;
--let $step = maintenance
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE th REMOVE PARTITIONING;
--let $step = remove_partitioning
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
DROP TABLE th;

--echo
--echo #### KEY
--let $ptype = KEY
--let $tbl = tk
eval CREATE TABLE tk (a INT NOT NULL, b INT, c VARCHAR(20), PRIMARY KEY (a), KEY kb (b))
  ENGINE=$engine PARTITION BY KEY (a) PARTITIONS 3;
INSERT INTO tk WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 39)
  SELECT n, n MOD 7, CONCAT('v', n) FROM s;
--let $step = create
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tk ADD PARTITION PARTITIONS 2;
--let $step = add
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tk COALESCE PARTITION 2;
--let $step = coalesce
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tk TRUNCATE PARTITION p2;
--let $step = truncate
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE tk PARTITION BY LINEAR KEY (a) PARTITIONS 4;
--let $step = repartition_linear_key
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
DROP TABLE tk;

--echo
--echo #### RANGE with HASH subpartitions
--let $ptype = RANGE_SUB
--let $tbl = ts
eval CREATE TABLE ts (a INT NOT NULL, b INT, c VARCHAR(20), PRIMARY KEY (a), KEY kb (b))
  ENGINE=$engine PARTITION BY RANGE (a) SUBPARTITION BY HASH (a) SUBPARTITIONS 2
  (PARTITION p0 VALUES LESS THAN (10), PARTITION p1 VALUES LESS THAN (20),
   PARTITION p2 VALUES LESS THAN (40));
INSERT INTO ts WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 39)
  SELECT n, n MOD 7, CONCAT('v', n) FROM s;
--let $step = create
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE ts ADD PARTITION (PARTITION p3 VALUES LESS THAN (50));
INSERT INTO ts VALUES (42, 0, 'v42'), (47, 5, 'v47');
--let $step = add
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE ts REORGANIZE PARTITION p0, p1 INTO (PARTITION p01 VALUES LESS THAN (20));
--let $step = reorganize_merge
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE ts TRUNCATE PARTITION p01sp1;
--let $step = truncate_subpartition
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
ALTER TABLE ts TRUNCATE PARTITION p2;
--let $step = truncate_partition
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
--echo # Drop the last partition: no remaining partition accepts its rows.
ALTER TABLE ts DROP PARTITION p3;
--let $step = drop_last
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
--replace_column 1 x
ALTER TABLE ts ANALYZE PARTITION p01;
--replace_column 1 x
ALTER TABLE ts CHECK PARTITION ALL;
--let $step = maintenance
--source suite/rocksdb/include/partition_ddl_matrix_check.inc
DROP TABLE ts;
