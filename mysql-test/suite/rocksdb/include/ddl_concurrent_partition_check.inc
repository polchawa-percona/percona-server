# Checks the state left by ddl_concurrent_partition.test.
#
# Expected contents:
#   t1..t4: rows 0..299 in p0..p2, b = a + $iterations
#   t5:     rows 0..299, b = a for a < 200, b = a + $iterations for a >= 200
#   h1:     rows 0..399, b = a + $iterations
# Every secondary index must return the same rows as the primary key, and no
# row may sit in a partition whose range excludes it.

--echo # Contents through the primary key and through the secondary index.
--disable_query_log
let $tables = t1 t2 t3 t4 t5 h1;
while ($tables)
{
  let $t = `SELECT SUBSTRING_INDEX('$tables', ' ', 1)`;
  let $tables = `SELECT TRIM(SUBSTRING('$tables', LENGTH('$t') + 1))`;
  let $pk = `SELECT CONCAT(COUNT(*), ' ', SUM(b), ' ', SUM(CRC32(CONCAT(a, ':', b, ':', c)))) FROM $t FORCE INDEX (PRIMARY)`;
  let $sk = `SELECT CONCAT(COUNT(*), ' ', SUM(b), ' ', SUM(CRC32(a + b * 1000))) FROM $t FORCE INDEX (kb)`;
  let $sk_from_pk = `SELECT CONCAT(COUNT(*), ' ', SUM(b), ' ', SUM(CRC32(a + b * 1000))) FROM $t FORCE INDEX (PRIMARY)`;
  let $expected = `SELECT CASE '$t'
    WHEN 't5' THEN CONCAT(300, ' ', 44850 + 100 * $iterations)
    WHEN 'h1' THEN CONCAT(400, ' ', 79800 + 400 * $iterations)
    ELSE CONCAT(300, ' ', 44850 + 300 * $iterations) END`;
  let $got = `SELECT SUBSTRING_INDEX('$pk', ' ', 2)`;
  if ($got != $expected)
  {
    --echo $t: count and SUM(b) $got, expected $expected
  }
  if ($got == $expected)
  {
    --echo $t: count and SUM(b) as expected
  }
  if ($sk != $sk_from_pk)
  {
    --echo $t: secondary index kb ($sk) differs from primary key ($sk_from_pk)
  }
}

--echo # Rows outside the range of their partition (expect none).
let $tables = t1 t2 t3 t4 t5;
while ($tables)
{
  let $t = `SELECT SUBSTRING_INDEX('$tables', ' ', 1)`;
  let $tables = `SELECT TRIM(SUBSTRING('$tables', LENGTH('$t') + 1))`;
  let $bad = `SELECT
      (SELECT COUNT(*) FROM $t PARTITION (p0) WHERE a < 0 OR a >= 100)
    + (SELECT COUNT(*) FROM $t PARTITION (p1) WHERE a < 100 OR a >= 200)
    + (SELECT COUNT(*) FROM $t PARTITION (p2) WHERE a < 200 OR a >= 300)`;
  let $parts = `SELECT GROUP_CONCAT(PARTITION_NAME ORDER BY PARTITION_ORDINAL_POSITION)
    FROM INFORMATION_SCHEMA.PARTITIONS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$t'`;
  --echo $t: partitions $parts, misplaced rows $bad
}
let $bad = `SELECT COUNT(*) FROM h1 WHERE a < 0 OR a >= 400`;
let $parts = `SELECT COUNT(*) FROM INFORMATION_SCHEMA.PARTITIONS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 'h1'`;
--echo h1: $parts partitions, rows out of range $bad

CHECK TABLE t1, t2, t3, t4, t5, h1;

--echo # Wait for the drop-index thread, then compact every column family.
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
SET GLOBAL rocksdb_signal_drop_index_thread = 1;
let $wait_timeout = 600;
let $wait_condition = SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ROCKSDB_GLOBAL_INFO WHERE TYPE = 'DDL_DROP_INDEX_ONGOING';
--source include/wait_condition.inc
SET GLOBAL rocksdb_compact_cf = 'default';
SET GLOBAL rocksdb_compact_cf = 'cf_part_shared';
SET GLOBAL rocksdb_compact_cf = 'cf_part_shared_sk';
SET GLOBAL rocksdb_compact_cf = 'cf_t3_p1';
SET GLOBAL rocksdb_compact_cf = 'rev:cf_t3_p2';
SET GLOBAL rocksdb_compact_cf = 'cf_t4_p1';
SET GLOBAL rocksdb_compact_cf = 'cf_t4_p3';

--echo # Index data in SST files that belongs to no index (expect 0).
let $orphans = `SELECT COUNT(*) FROM INFORMATION_SCHEMA.ROCKSDB_INDEX_FILE_MAP m
  WHERE m.INDEX_NUMBER >= 256
    AND NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
                     WHERE d.COLUMN_FAMILY = m.COLUMN_FAMILY
                       AND d.INDEX_NUMBER = m.INDEX_NUMBER)`;
--echo orphan index entries in SST files: $orphans
if ($orphans)
{
  SELECT m.COLUMN_FAMILY, m.INDEX_NUMBER, SUM(m.NUM_ROWS) AS rows_in_sst
    FROM INFORMATION_SCHEMA.ROCKSDB_INDEX_FILE_MAP m
   WHERE m.INDEX_NUMBER >= 256
     AND NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
                      WHERE d.COLUMN_FAMILY = m.COLUMN_FAMILY
                        AND d.INDEX_NUMBER = m.INDEX_NUMBER)
   GROUP BY m.COLUMN_FAMILY, m.INDEX_NUMBER;
}
--echo # MyRocks indexes per table (one per partition per index).
SELECT TABLE_NAME, COUNT(*) FROM INFORMATION_SCHEMA.ROCKSDB_DDL
 WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME <> 'seq'
 GROUP BY TABLE_NAME ORDER BY TABLE_NAME;
--enable_query_log
