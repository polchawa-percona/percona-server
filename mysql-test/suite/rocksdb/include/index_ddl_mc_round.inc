# One round of t/index_ddl_manual_compaction.test with the manual compaction
# selected by $mc_trigger (see index_ddl_mc_trigger.inc).
--disable_query_log
DROP TABLE IF EXISTS ta, tn, th, tr, tp, tz, tzr;
CREATE TABLE ta (pk INT PRIMARY KEY, a INT NOT NULL,
  b VARCHAR(32) COLLATE utf8mb4_0900_ai_ci NOT NULL, c TEXT,
  KEY ka0 (a), KEY kc0 (c(8)), KEY kba0 (b, a)) ENGINE=ROCKSDB;
INSERT INTO ta WITH RECURSIVE v AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM v WHERE n < 3000)
  SELECT n, n % 500, CONCAT('v', n), CONCAT(REPEAT(CHAR(65 + n % 26), 12), n) FROM v;
CREATE TABLE tn (pk INT PRIMARY KEY, a INT NOT NULL, b VARCHAR(32) NOT NULL, KEY ka (a)) ENGINE=ROCKSDB;
INSERT INTO tn WITH RECURSIVE v AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM v WHERE n < 3000)
  SELECT n, n % 700, CONCAT('n', n) FROM v;
CREATE TABLE th (a INT NOT NULL, b VARCHAR(32) NOT NULL, KEY ka (a)) ENGINE=ROCKSDB;
INSERT INTO th WITH RECURSIVE v AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM v WHERE n < 3000)
  SELECT n % 300, CONCAT('h', n) FROM v;
CREATE TABLE tr (pk INT NOT NULL, a INT NOT NULL, b VARCHAR(32) NOT NULL,
  PRIMARY KEY (pk) COMMENT 'cfname=rev:mc2r', KEY ka (a) COMMENT 'cfname=rev:mc2r') ENGINE=ROCKSDB;
INSERT INTO tr WITH RECURSIVE v AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM v WHERE n < 3000)
  SELECT n, n % 400, CONCAT('r', n) FROM v;
CREATE TABLE tp (i INT NOT NULL, j INT NOT NULL, k INT NOT NULL, PRIMARY KEY (i, j)) ENGINE=ROCKSDB;
INSERT INTO tp WITH RECURSIVE v AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM v WHERE n < 3000)
  SELECT n % 50, n, n % 13 FROM v;
# tz and tzr are only compacted (OPTIMIZE TABLE), never read or altered.
CREATE TABLE tz (pk INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=ROCKSDB;
INSERT INTO tz WITH RECURSIVE v AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM v WHERE n < 3000)
  SELECT n, n % 900 FROM v;
CREATE TABLE tzr (pk INT NOT NULL, a INT, PRIMARY KEY (pk) COMMENT 'cfname=rev:mc2r') ENGINE=ROCKSDB;
INSERT INTO tzr WITH RECURSIVE v AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM v WHERE n < 3000)
  SELECT n, n % 900 FROM v;
SET GLOBAL rocksdb_force_flush_memtable_now = 1;
--enable_query_log
--let $mc_optimize_table = tz, tzr

--echo # S1: ADD INDEX + ADD UNIQUE INDEX paused in the PK scan (after its snapshot)
--let $mc_old = `SELECT CONCAT(COUNT(*), ':', SUM(a)) FROM ta`
connection con_s;
# At READ COMMITTED the server warns that no snapshot is taken.
--disable_warnings
START TRANSACTION WITH CONSISTENT SNAPSHOT;
--enable_warnings
--disable_result_log
SELECT COUNT(*) FROM tp;
--enable_result_log
connection default;
UPDATE ta SET a = a + 1000 WHERE pk <= 100;
--let $mc_new = `SELECT CONCAT(COUNT(*), ':', SUM(a)) FROM ta`
connection con1;
SET DEBUG_SYNC = 'rocksdb.inplace_populate_sk_scan_row SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE ta ADD INDEX k1 (a, b), ADD UNIQUE INDEX u1 (b), ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--source suite/rocksdb/include/index_ddl_mc_reader.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--reap
connection default;
--source suite/rocksdb/include/index_ddl_mc_trigger.inc
connection con_s;
--let $mc_s_pk = `SELECT CONCAT(COUNT(*), ':', SUM(a)) FROM ta FORCE INDEX (PRIMARY)`
--let $mc_s_sk = `SELECT CONCAT(COUNT(*), ':', SUM(a)) FROM ta FORCE INDEX (ka0)`
# A read through k1, created after the snapshot, is not checked here (PS-11627).
COMMIT;
--let $mc_exp = $mc_old
if ($mc_iso == READ-COMMITTED)
{
  --let $mc_exp = $mc_new
}
if ($mc_s_pk == $mc_exp)
{
  if ($mc_s_sk == $mc_exp)
  {
    --echo # snapshot older than the ALTER: PK and old SK return the expected rows
  }
}
if ($mc_s_pk != $mc_exp)
{
  --echo # MISMATCH snapshot PK read $mc_s_pk, expected $mc_exp
}
if ($mc_s_sk != $mc_exp)
{
  --echo # MISMATCH snapshot SK read $mc_s_sk, expected $mc_exp
}
connection default;
SET DEBUG_SYNC = 'RESET';

--echo # S2: hidden-PK table, ADD INDEX paused before the merge
connection con1;
SET DEBUG_SYNC = 'rocksdb.inplace_populate_sk_before_merge SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE th ADD INDEX kb (b), ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--source suite/rocksdb/include/index_ddl_mc_reader.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--reap
connection default;
SET DEBUG_SYNC = 'RESET';

--echo # S3: reverse CF, ADD INDEX paused before the ingest
connection con1;
SET DEBUG_SYNC = 'rocksdb.bulk_load_before_ingest SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE tr ADD INDEX kab (a, b) COMMENT 'cfname=rev:mc2r', ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--source suite/rocksdb/include/index_ddl_mc_reader.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--reap
connection default;
SET DEBUG_SYNC = 'RESET';

--echo # S4: two indexes in one ALTER, paused after the first one is ingested
connection con1;
SET DEBUG_SYNC = 'rocksdb.inplace_populate_sk_after_ingest SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE ta ADD INDEX k2 (c(8), a), ADD INDEX k3 (b(4)), ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--source suite/rocksdb/include/index_ddl_mc_reader.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--reap
connection default;
SET DEBUG_SYNC = 'RESET';

--echo # S5: partial index and a plain index, paused after the dictionary commit
connection con1;
SET DEBUG_SYNC = 'rocksdb.commit_in_place_alter_table_dict_committed SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE tp ADD INDEX ik (i, k) COMMENT 'partial_group_keyparts=1;partial_group_threshold=5', ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--let $mc_optimize_table = ta
--source suite/rocksdb/include/index_ddl_mc_trigger.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--reap
SET DEBUG_SYNC = 'rocksdb.commit_in_place_alter_table_dict_committed SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE tn ADD INDEX kb (b), ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--let $mc_optimize_table = ta
--source suite/rocksdb/include/index_ddl_mc_trigger.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--reap
connection default;
SET DEBUG_SYNC = 'RESET';
--let $mc_optimize_table = tz, tzr

--echo # S6: UNIQUE build with duplicates next to a good index, paused after the first build
connection con1;
SET DEBUG_SYNC = 'rocksdb.inplace_populate_sk_after_ingest SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE ta ADD INDEX k4 (a, c(4)), ADD UNIQUE INDEX u4 (a), ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--source suite/rocksdb/include/index_ddl_mc_trigger.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--replace_regex /#sql-[0-9a-f_]*/#sql-temporary/
--error ER_DUP_ENTRY
--reap
--replace_regex /#sql-[0-9a-f_]*/#sql-temporary/
--error ER_DUP_ENTRY
ALTER TABLE ta ADD UNIQUE INDEX u5 (a), ALGORITHM=INPLACE;
connection default;
SET DEBUG_SYNC = 'RESET';
--source suite/rocksdb/include/index_ddl_mc_trigger.inc

--echo # S7: DROP INDEX while the drop-index thread is held, next to live indexes
SET GLOBAL debug = '+d,rocksdb_pause_drop_index_thread';
ALTER TABLE ta DROP INDEX k1, ALGORITHM=INPLACE;
ALTER TABLE tn ADD INDEX kadj (a, b), ALGORITHM=INPLACE;
ALTER TABLE tr DROP INDEX ka, ADD INDEX ka (a) COMMENT 'cfname=rev:mc2r', ALGORITHM=INPLACE;
--let $mc_between = CALL mc_dml()
--source suite/rocksdb/include/index_ddl_mc_reader.inc
connection con1;
SET DEBUG_SYNC = 'rocksdb.bulk_load_before_ingest SIGNAL p1 WAIT_FOR g1';
--send ALTER TABLE ta DROP INDEX kc0, ADD INDEX kc0 (c(10)), ALGORITHM=INPLACE
connection default;
SET DEBUG_SYNC = 'now WAIT_FOR p1';
--source suite/rocksdb/include/index_ddl_mc_trigger.inc
SET DEBUG_SYNC = 'now SIGNAL g1';
connection con1;
--reap
connection default;
SET DEBUG_SYNC = 'RESET';
--let $mc_optimize_table = ta
--source suite/rocksdb/include/index_ddl_mc_trigger.inc
--source suite/rocksdb/include/index_ddl_mc_check.inc
SET GLOBAL debug = '-d,rocksdb_pause_drop_index_thread';

--echo # checks
--source suite/rocksdb/include/index_ddl_mc_check.inc
--disable_query_log
--let $ccic_table = ta
--let $ccic_col = b
--let $ccic_key = u1
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--let $ccic_key = k3
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--let $ccic_col = c
--let $ccic_key = k2
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--let $ccic_table = th
--let $ccic_col = b
--let $ccic_key = kb
--let $mcc_base = IGNORE INDEX (ka, kb)
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--let $mcc_base =
--let $ccic_table = tr
--let $ccic_key = kab
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--let $ccic_table = tn
--let $ccic_key = kb
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--let $ccic_key = kadj
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--let $ccic_table = tp
--let $ccic_col = k
--let $ccic_key = ik
--source suite/rocksdb/include/index_ddl_mc_idx.inc
--enable_query_log
SELECT INDEX_NAME, NON_UNIQUE FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 'ta' AND SEQ_IN_INDEX = 1 ORDER BY INDEX_NAME;
CHECK TABLE ta, tn, th, tr, tp, tz, tzr;
--source suite/rocksdb/include/index_ddl_mc_reclaimed.inc
