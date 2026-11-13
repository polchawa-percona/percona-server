# Body of rocksdb.stats_settings_concurrent_ddl and
# rocksdb.stats_row_cap_concurrent_ddl: runs the settings profiles
# $profile_first..$profile_last, except $profile_skip.
--disable_query_log
SET @save_use_table_scan = @@global.rocksdb_table_stats_use_table_scan;
SET @save_skip_system_cf = @@global.rocksdb_table_stats_skip_system_cf;
SET @save_max_rows = @@global.rocksdb_table_stats_max_num_rows_scanned;
SET @save_threshold_count = @@global.rocksdb_table_stats_recalc_threshold_count;
SET @save_threshold_pct = @@global.rocksdb_table_stats_recalc_threshold_pct;
SET @save_sampling_pct = @@global.rocksdb_table_stats_sampling_pct;
SET @save_seconds = @@global.rocksdb_seconds_between_stat_computes;
SET @save_recalc_rate = @@global.rocksdb_stats_recalc_rate;
SET @save_force_mem = @@global.rocksdb_force_compute_memtable_stats;
SET @save_force_mem_cache = @@global.rocksdb_force_compute_memtable_stats_cachetime;
--enable_query_log


CREATE TABLE seq (n INT PRIMARY KEY) ENGINE=ROCKSDB;
SET SESSION cte_max_recursion_depth = 10000;
INSERT INTO seq
  WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM s WHERE n < 1999)
  SELECT n FROM s;

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);
connect (con3,localhost,root,,);
connect (con4,localhost,root,,);
connect (con5,localhost,root,,);
connection default;

--let $profile = $profile_first
while ($profile <= $profile_last)
{
if ($profile != $profile_skip)
{
  # Every profile starts from the defaults saved above.
  --disable_query_log
  SET GLOBAL rocksdb_table_stats_use_table_scan = @save_use_table_scan;
  SET GLOBAL rocksdb_table_stats_skip_system_cf = @save_skip_system_cf;
  SET GLOBAL rocksdb_table_stats_max_num_rows_scanned = @save_max_rows;
  SET GLOBAL rocksdb_table_stats_recalc_threshold_count = @save_threshold_count;
  SET GLOBAL rocksdb_table_stats_recalc_threshold_pct = @save_threshold_pct;
  SET GLOBAL rocksdb_table_stats_sampling_pct = @save_sampling_pct;
  SET GLOBAL rocksdb_seconds_between_stat_computes = @save_seconds;
  SET GLOBAL rocksdb_stats_recalc_rate = @save_recalc_rate;
  SET GLOBAL rocksdb_force_compute_memtable_stats = @save_force_mem;
  SET GLOBAL rocksdb_force_compute_memtable_stats_cachetime = @save_force_mem_cache;
  --enable_query_log
  --let $concurrent_analyze = 1

  if ($profile == 1)
  {
    --echo # Profile 1: defaults
  }
  if ($profile == 2)
  {
    --echo # Profile 2: table-scan statistics, recalculation on every change
    SET GLOBAL rocksdb_table_stats_use_table_scan = ON;
    SET GLOBAL rocksdb_table_stats_recalc_threshold_count = 0;
    SET GLOBAL rocksdb_table_stats_recalc_threshold_pct = 0;
    SET GLOBAL rocksdb_table_stats_sampling_pct = 100;
    SET GLOBAL rocksdb_stats_recalc_rate = 100;
    --let $concurrent_analyze = 0
  }
  if ($profile == 3)
  {
    --echo # Profile 3: table-scan statistics with a row cap
    SET GLOBAL rocksdb_table_stats_use_table_scan = ON;
    SET GLOBAL rocksdb_table_stats_max_num_rows_scanned = 500;
    SET GLOBAL rocksdb_table_stats_sampling_pct = 100;
    --let $concurrent_analyze = 0
  }
  if ($profile == 4)
  {
    --echo # Profile 4: skip system CF, no forced memtable statistics
    SET GLOBAL rocksdb_table_stats_skip_system_cf = ON;
    SET GLOBAL rocksdb_force_compute_memtable_stats = OFF;
    SET GLOBAL rocksdb_force_compute_memtable_stats_cachetime = 0;
  }
  if ($profile == 5)
  {
    --echo # Profile 5: SST-property statistics, minimal sampling, continuous recompute
    SET GLOBAL rocksdb_table_stats_sampling_pct = 1;
    SET GLOBAL rocksdb_seconds_between_stat_computes = 0;
    SET GLOBAL rocksdb_stats_recalc_rate = 1;
  }
  if ($profile == 6)
  {
    --echo # Profile 6: SST-property statistics, full sampling, fast recalculation
    SET GLOBAL rocksdb_table_stats_sampling_pct = 100;
    SET GLOBAL rocksdb_stats_recalc_rate = 100;
  }

  --let $t = 1
  while ($t <= 4)
  {
    --disable_query_log
    --eval CREATE TABLE t$t (pk INT PRIMARY KEY, a INT, b INT, KEY ka (a), KEY kb (b)) ENGINE=ROCKSDB
    --eval INSERT INTO t$t SELECT n, n % 100, n FROM seq
    --enable_query_log
    --inc $t
  }
  SET GLOBAL rocksdb_force_flush_memtable_now = 1;

  --let $round = 0
  while ($round < 3)
  {
    --disable_query_log
    connection con1;
    send TRUNCATE TABLE t1;
    connection con2;
    send RENAME TABLE t2 TO t2_moved;
    connection con3;
    send ALTER TABLE t3 ADD INDEX kab (a, b), ALGORITHM=INPLACE;
    connection con4;
    send DROP TABLE t4;
    if ($concurrent_analyze)
    {
      connection con5;
      send ANALYZE TABLE t1, t3, seq;
    }

    connection con1;
    reap;
    send INSERT INTO t1 SELECT n, n % 100, n FROM seq;
    connection con2;
    reap;
    send RENAME TABLE t2_moved TO t2;
    connection con3;
    reap;
    send ALTER TABLE t3 DROP INDEX kab, ALGORITHM=INPLACE;
    connection con4;
    reap;
    send CREATE TABLE t4 (pk INT PRIMARY KEY, a INT, b INT, KEY ka (a), KEY kb (b)) ENGINE=ROCKSDB;
    if ($concurrent_analyze)
    {
      connection con5;
      reap;
      send ANALYZE TABLE t2, t4, seq;
    }

    connection con1;
    reap;
    connection con2;
    reap;
    send ANALYZE TABLE t2;
    connection con3;
    reap;
    send ALTER TABLE t3 FORCE, ALGORITHM=COPY;
    connection con4;
    reap;
    send INSERT INTO t4 SELECT n, n % 100, n FROM seq;
    if ($concurrent_analyze)
    {
      connection con5;
      --error 0,ER_NO_SUCH_TABLE
      reap;
    }

    connection con2;
    reap;
    connection con3;
    reap;
    connection con4;
    reap;
    connection default;
    --enable_query_log
    --inc $round
  }

  SET GLOBAL rocksdb_force_flush_memtable_now = 1;
  --disable_result_log
  ANALYZE TABLE t1, t2, t3, t4;
  --enable_result_log

  --echo # Statistics outside the expected range (no rows expected):
  --echo # row count within 50% of the 2000 loaded rows; every index has a
  --echo # cardinality, at most the row count.
  # Without SST properties (rocksdb_collect_sst_properties=OFF) MyRocks has no
  # row-count source after a flush, so only the data is checked; see PS-11662.
  SELECT table_name, table_rows
    FROM information_schema.tables
    WHERE table_schema = 'test' AND table_name IN ('t1','t2','t3','t4')
      AND @@global.rocksdb_collect_sst_properties
      AND NOT (table_rows BETWEEN 1000 AND 3000)
    ORDER BY table_name;
  SELECT table_name, index_name, cardinality
    FROM information_schema.statistics
    WHERE table_schema = 'test' AND table_name IN ('t1','t2','t3','t4')
      AND @@global.rocksdb_collect_sst_properties
      AND NOT (cardinality IS NOT NULL AND cardinality > 0
               AND cardinality <= 3000)
    ORDER BY table_name, index_name;
  --echo # Data intact
  SELECT COUNT(*), SUM(pk) FROM t1;
  SELECT COUNT(*), SUM(pk) FROM t2;
  SELECT COUNT(*), SUM(pk) FROM t3;
  SELECT COUNT(*), SUM(pk) FROM t4;

  DROP TABLE t1, t2, t3, t4;
}
  --inc $profile
}

--disable_query_log
SET GLOBAL rocksdb_table_stats_use_table_scan = @save_use_table_scan;
SET GLOBAL rocksdb_table_stats_skip_system_cf = @save_skip_system_cf;
SET GLOBAL rocksdb_table_stats_max_num_rows_scanned = @save_max_rows;
SET GLOBAL rocksdb_table_stats_recalc_threshold_count = @save_threshold_count;
SET GLOBAL rocksdb_table_stats_recalc_threshold_pct = @save_threshold_pct;
SET GLOBAL rocksdb_table_stats_sampling_pct = @save_sampling_pct;
SET GLOBAL rocksdb_seconds_between_stat_computes = @save_seconds;
SET GLOBAL rocksdb_stats_recalc_rate = @save_recalc_rate;
SET GLOBAL rocksdb_force_compute_memtable_stats = @save_force_mem;
SET GLOBAL rocksdb_force_compute_memtable_stats_cachetime = @save_force_mem_cache;
--enable_query_log

disconnect con1;
disconnect con2;
disconnect con3;
disconnect con4;
disconnect con5;
DROP TABLE seq;
