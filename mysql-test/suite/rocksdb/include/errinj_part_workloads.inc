#
# Runs errinj_loop.inc over the partitioning workloads for one class and
# status. Needs errinj_part_setup.inc, $errinj_class and $errinj_status.
# $errinj_part_ddl: 0 or unset runs the DML workloads, 1 the DDL workloads.
# EXCHANGE PARTITION is not supported by MyRocks (PS-11699) and not run.
#

if (!$errinj_part_ddl) {
  --let $errinj_stmt = INSERT INTO th VALUES (40, 40, 'n'), (41, 41, 'n'), (42, 42, 'n'), (43, 43, 'n')
  --let $errinj_label = $errinj_class/$errinj_status INSERT into every HASH partition
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = UPDATE tp SET pk = pk + 11 WHERE pk IN (0, 12, 24)
  --let $errinj_label = $errinj_class/$errinj_status UPDATE moving rows between partitions
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = UPDATE tp SET b = CONCAT(b, 'u'), a = a + 1 WHERE pk BETWEEN 12 AND 27
  --let $errinj_label = $errinj_class/$errinj_status UPDATE in two partitions
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = DELETE FROM tp WHERE pk IN (1, 13, 22, 34)
  --let $errinj_label = $errinj_class/$errinj_status DELETE in every partition
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = REPLACE INTO th VALUES (1, 500, 'r'), (2, 500, 'r'), (77, 500, 'r')
  --let $errinj_label = $errinj_class/$errinj_status REPLACE into HASH partitions
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = INSERT INTO t_outp SELECT * FROM tp WHERE pk BETWEEN 12 AND 17
  --let $errinj_label = $errinj_class/$errinj_status pruned range read
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = INSERT INTO t_outp SELECT * FROM tp PARTITION (p1, p3) WHERE a MOD 2 = 0
  --let $errinj_label = $errinj_class/$errinj_status read of explicit partitions
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = INSERT INTO t_outp SELECT STRAIGHT_JOIN th.* FROM tp JOIN th FORCE INDEX (PRIMARY) ON th.pk = tp.a WHERE tp.pk BETWEEN 5 AND 14
  --let $errinj_label = $errinj_class/$errinj_status point reads into HASH partitions
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_part_ddl) {
  --let $errinj_stmt = ALTER TABLE tp ADD PARTITION (PARTITION p4 VALUES LESS THAN (50))
  --let $errinj_label = $errinj_class/$errinj_status ADD PARTITION
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = ALTER TABLE tp DROP PARTITION p1
  --let $errinj_label = $errinj_class/$errinj_status DROP PARTITION
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = ALTER TABLE tp TRUNCATE PARTITION p2
  --let $errinj_label = $errinj_class/$errinj_status TRUNCATE PARTITION
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = ALTER TABLE tp REORGANIZE PARTITION p2, p3 INTO (PARTITION p23 VALUES LESS THAN (40))
  --let $errinj_label = $errinj_class/$errinj_status REORGANIZE PARTITION
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = ALTER TABLE th COALESCE PARTITION 2
  --let $errinj_label = $errinj_class/$errinj_status COALESCE PARTITION
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = ALTER TABLE th ADD PARTITION PARTITIONS 2
  --let $errinj_label = $errinj_class/$errinj_status ADD HASH PARTITIONS
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = ALTER TABLE tp ADD INDEX ka (a), ALGORITHM=INPLACE
  --let $errinj_label = $errinj_class/$errinj_status INPLACE ADD INDEX on a partitioned table
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = ALTER TABLE tp OPTIMIZE PARTITION p1, p2
  --let $errinj_label = $errinj_class/$errinj_status OPTIMIZE PARTITION
  --source suite/rocksdb/include/errinj_loop.inc
}
