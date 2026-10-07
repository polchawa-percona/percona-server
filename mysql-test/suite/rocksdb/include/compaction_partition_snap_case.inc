# One case of compaction_partition_snapshot.test.
#   $sc_level:   REPEATABLE-READ or READ-COMMITTED (connection con_snap)
#   $sc_op:      TRUNCATE or REORGANIZE (table ts, partition p1), DROP
#                (table ts, partition p2: dropping p1 would hit PS-11634,
#                its rows would move into p2) or COALESCE (table hs)
#   $sc_trigger: optimize_table, optimize_partition, compact_cf, lzero or
#                flush_lzero
# con_snap starts a transaction (with a consistent snapshot at RR) without
# touching the tables, so it holds no metadata lock. Then the default
# connection runs the partition DDL, changes a row in an untouched partition,
# and runs the manual compaction. con_snap then reads the affected and the
# untouched partitions.
--disable_query_log
CREATE TABLE ts (
  id INT NOT NULL, u INT, c VARCHAR(32),
  PRIMARY KEY (id) COMMENT 'p1_cfname=mc4_s1;p2_cfname=mc4_s2',
  KEY ku (u) COMMENT 'p1_cfname=mc4_s1_sk;p2_cfname=mc4_s2'
) ENGINE = ROCKSDB PARTITION BY RANGE (id) (
  PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN (200),
  PARTITION p2 VALUES LESS THAN (300));
INSERT INTO ts SELECT n, n, CONCAT('c', n) FROM seq WHERE n < 300;
CREATE TABLE hs (id INT NOT NULL PRIMARY KEY, u INT, KEY ku (u))
  ENGINE = ROCKSDB PARTITION BY HASH (id) PARTITIONS 4;
INSERT INTO hs SELECT n, n FROM seq WHERE n < 200;
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
let $sc_part = p1;
let $sc_range = id BETWEEN 100 AND 199;
let $sc_urange = u BETWEEN 100 AND 199;
let $sc_others = p0, p2;
let $sc_untouched = (id < 100 OR id >= 200);
let $sc_cf = mc4_s1;
if ($sc_op == DROP)
{
  let $sc_part = p2;
  let $sc_range = id BETWEEN 200 AND 299;
  let $sc_urange = u BETWEEN 200 AND 299;
  let $sc_others = p0, p1;
  let $sc_untouched = (id < 200);
  let $sc_cf = mc4_s2;
}
let $sc_sum = CONCAT(COUNT(*), ':', COALESCE(SUM(CRC32(CONCAT_WS('#', id, u, c))), 0));
let $sc_hsum = CONCAT(COUNT(*), ':', COALESCE(SUM(CRC32(CONCAT_WS('#', id, u))), 0));
let $sc_exp_old = `SELECT $sc_sum FROM ts WHERE $sc_untouched`;
let $sc_hexp_old = `SELECT $sc_hsum FROM hs`;

connection con_snap;
eval SET SESSION transaction_isolation = '$sc_level';
if ($sc_level == REPEATABLE-READ)
{
  START TRANSACTION WITH CONSISTENT SNAPSHOT;
}
if ($sc_level != REPEATABLE-READ)
{
  START TRANSACTION;
}

connection default;
--disable_warnings
if ($sc_op == TRUNCATE)
{
  ALTER TABLE ts TRUNCATE PARTITION p1;
}
if ($sc_op == DROP)
{
  ALTER TABLE ts DROP PARTITION p2;
}
if ($sc_op == REORGANIZE)
{
  ALTER TABLE ts REORGANIZE PARTITION p1 INTO (PARTITION p1 VALUES LESS THAN (150), PARTITION p1b VALUES LESS THAN (200));
}
if ($sc_op == COALESCE)
{
  ALTER TABLE hs COALESCE PARTITION 2;
}
--enable_warnings
UPDATE ts SET u = u + 1000 WHERE id IN (5, 105, 205);
UPDATE hs SET u = u + 1000 WHERE id = 3;

let $sc_opt_part = $sc_part;
if ($sc_op == DROP)
{
  let $sc_opt_part = p0;
}
let $sc_table = ts;
if ($sc_op == COALESCE)
{
  let $sc_table = hs;
  let $sc_cf = default;
  let $sc_opt_part = p0;
}
if ($sc_trigger == optimize_table)
{
  --disable_result_log
  eval OPTIMIZE TABLE $sc_table;
  --enable_result_log
}
if ($sc_trigger == optimize_partition)
{
  --disable_result_log
  eval ALTER TABLE $sc_table OPTIMIZE PARTITION $sc_opt_part;
  --enable_result_log
}
if ($sc_trigger == compact_cf)
{
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  eval SET GLOBAL rocksdb_compact_cf = '$sc_cf';
}
if ($sc_trigger == lzero)
{
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  --disable_result_log
  --error 0, ER_WRONG_VALUE_FOR_VAR
  SET GLOBAL rocksdb_compact_lzero_now = 1;
  --enable_result_log
}
if ($sc_trigger == flush_lzero)
{
  --disable_result_log
  --error 0, ER_WRONG_VALUE_FOR_VAR
  SET GLOBAL rocksdb_force_flush_memtable_and_lzero_now = 1;
  --enable_result_log
}
let $sc_exp_new = `SELECT $sc_sum FROM ts WHERE $sc_untouched`;
let $sc_hexp_new = `SELECT $sc_hsum FROM hs`;

connection con_snap;
if ($sc_op != COALESCE)
{
  let $sr_q = SELECT $sc_sum FROM ts PARTITION ($sc_part);
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_pa = $sr_out;
  let $sr_q = SELECT $sc_sum FROM ts FORCE INDEX (PRIMARY) WHERE $sc_range;
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_pk = $sr_out;
  let $sr_q = SELECT COUNT(*) FROM ts FORCE INDEX (ku) WHERE $sc_urange;
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_sk = $sr_out;
  let $sr_q = SELECT $sc_sum FROM ts PARTITION ($sc_others);
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_un = $sr_out;
  let $sr_q = SELECT $sc_sum FROM ts WHERE $sc_untouched;
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_unp = $sr_out;
  if ($sc_un == $sc_exp_old)
  {
    let $sc_un = snapshot;
  }
  if ($sc_un == $sc_exp_new)
  {
    let $sc_un = current;
  }
  if ($sc_un == 0:0)
  {
    let $sc_un = empty;
  }
  if ($sc_unp == $sc_exp_old)
  {
    let $sc_unp = snapshot;
  }
  if ($sc_unp == $sc_exp_new)
  {
    let $sc_unp = current;
  }
  if ($sc_unp == 0:0)
  {
    let $sc_unp = empty;
  }
  --echo $sc_level $sc_op $sc_part $sc_trigger: $sc_part $sc_pa, pk range $sc_pk, ku range $sc_sk, untouched ($sc_others) $sc_un, untouched pruned $sc_unp
}
if ($sc_op == COALESCE)
{
  let $sr_q = SELECT $sc_hsum FROM hs;
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_all = $sr_out;
  let $sr_q = SELECT $sc_hsum FROM hs PARTITION (p1);
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_hp1 = $sr_out;
  let $sr_q = SELECT COUNT(*) FROM hs FORCE INDEX (ku) WHERE u < 200;
  --source suite/rocksdb/include/compaction_partition_snap_read.inc
  let $sc_hk = $sr_out;
  if ($sc_all == $sc_hexp_old)
  {
    let $sc_all = snapshot;
  }
  if ($sc_all == $sc_hexp_new)
  {
    let $sc_all = current;
  }
  if ($sc_all == 0:0)
  {
    let $sc_all = empty;
  }
  --echo $sc_level $sc_op $sc_trigger: hs $sc_all, p1 $sc_hp1, ku u<200 $sc_hk
}
COMMIT;
connection default;
DROP TABLE ts, hs;
--enable_query_log
