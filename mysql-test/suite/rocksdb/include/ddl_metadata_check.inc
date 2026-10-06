# Checks that MyRocks metadata agrees with the server's view of the tables
# created by ddl_metadata_after_restart.test. Expects $md_base: the index
# number allocated just before the test's first table.

--echo # Index definitions in ROCKSDB_DDL
SELECT TABLE_SCHEMA, TABLE_NAME, PARTITION_NAME, INDEX_NAME, INDEX_TYPE,
       KV_FORMAT_VERSION, CF, TTL_DURATION, INDEX_FLAGS
  FROM INFORMATION_SCHEMA.ROCKSDB_DDL
 WHERE TABLE_SCHEMA IN ('test', 'md2')
 ORDER BY TABLE_SCHEMA, TABLE_NAME, PARTITION_NAME, INDEX_NAME;

--echo # Indexes known to the server but missing in ROCKSDB_DDL (expect none)
SELECT s.TABLE_SCHEMA, s.TABLE_NAME, s.INDEX_NAME
  FROM (SELECT DISTINCT TABLE_SCHEMA, TABLE_NAME, INDEX_NAME
          FROM INFORMATION_SCHEMA.STATISTICS
         WHERE TABLE_SCHEMA IN ('test', 'md2') AND TABLE_NAME LIKE 't\_%') s
 WHERE NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
                    WHERE d.TABLE_SCHEMA = s.TABLE_SCHEMA
                      AND d.TABLE_NAME = s.TABLE_NAME
                      AND d.INDEX_NAME = s.INDEX_NAME);

--echo # Indexes in ROCKSDB_DDL unknown to the server (expect none)
SELECT DISTINCT d.TABLE_SCHEMA, d.TABLE_NAME, d.INDEX_NAME
  FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
 WHERE d.TABLE_SCHEMA IN ('test', 'md2')
   AND NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.STATISTICS s
                    WHERE s.TABLE_SCHEMA = d.TABLE_SCHEMA
                      AND s.TABLE_NAME = d.TABLE_NAME
                      AND s.INDEX_NAME = d.INDEX_NAME)
 ORDER BY 1, 2, 3;

--echo # SST data of indexes that no table owns any more (expect none)
--replace_result $md_base MD_BASE
eval SELECT DISTINCT f.INDEX_NUMBER > 0 AS orphan_index
  FROM INFORMATION_SCHEMA.ROCKSDB_INDEX_FILE_MAP f
 WHERE f.INDEX_NUMBER > $md_base
   AND NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
                    WHERE d.INDEX_NUMBER = f.INDEX_NUMBER
                      AND d.COLUMN_FAMILY = f.COLUMN_FAMILY);

--echo # Indexes of non-empty tables without SST data (expect none)
SELECT d.TABLE_SCHEMA, d.TABLE_NAME, d.PARTITION_NAME, d.INDEX_NAME
  FROM INFORMATION_SCHEMA.ROCKSDB_DDL d
 WHERE d.TABLE_SCHEMA IN ('test', 'md2')
   AND d.TABLE_NAME LIKE 't\_%'
   AND NOT EXISTS (SELECT 1 FROM INFORMATION_SCHEMA.ROCKSDB_INDEX_FILE_MAP f
                    WHERE f.INDEX_NUMBER = d.INDEX_NUMBER
                      AND f.COLUMN_FAMILY = d.COLUMN_FAMILY)
 ORDER BY 1, 2, 3, 4;

--echo # Row counts through the primary key and each secondary index
SELECT 't_create' t, (SELECT COUNT(*) FROM t_create) pk,
       (SELECT COUNT(*) FROM t_create FORCE INDEX (kb)) sk;
SELECT 't_addidx' t, (SELECT COUNT(*) FROM t_addidx) pk,
       (SELECT COUNT(*) FROM t_addidx FORCE INDEX (kc)) sk;
SELECT 't_dropidx' t, (SELECT COUNT(*) FROM t_dropidx) pk,
       (SELECT COUNT(*) FROM t_dropidx FORCE INDEX (kb)) sk;
SELECT 't_rename' t, (SELECT COUNT(*) FROM t_rename) pk,
       (SELECT COUNT(*) FROM t_rename FORCE INDEX (kb)) sk;
SELECT 'md2.t_xdb' t, (SELECT COUNT(*) FROM md2.t_xdb) pk,
       (SELECT COUNT(*) FROM md2.t_xdb FORCE INDEX (kb)) sk;
SELECT 't_trunc' t, (SELECT COUNT(*) FROM t_trunc) pk,
       (SELECT COUNT(*) FROM t_trunc FORCE INDEX (kb)) sk;
SELECT 't_copy' t, (SELECT COUNT(*) FROM t_copy) pk,
       (SELECT COUNT(*) FROM t_copy FORCE INDEX (kb)) sk;
SELECT 't_instant' t, (SELECT COUNT(*) FROM t_instant) pk,
       (SELECT COUNT(*) FROM t_instant FORCE INDEX (kb)) sk,
       (SELECT SUM(e) FROM t_instant) e_sum;
SELECT 't_renidx' t, (SELECT COUNT(*) FROM t_renidx) pk,
       (SELECT COUNT(*) FROM t_renidx FORCE INDEX (kb2)) sk;
SELECT 't_pk' t, (SELECT COUNT(*) FROM t_pk) pk,
       (SELECT COUNT(*) FROM t_pk FORCE INDEX (kb)) sk;
SELECT 't_hidden' t, (SELECT COUNT(*) FROM t_hidden) pk,
       (SELECT COUNT(*) FROM t_hidden FORCE INDEX (kb)) sk;
SELECT 't_part' t, (SELECT COUNT(*) FROM t_part) pk,
       (SELECT COUNT(*) FROM t_part FORCE INDEX (kb)) sk;
SELECT 't_ttl' t, (SELECT COUNT(*) FROM t_ttl) pk,
       (SELECT COUNT(*) FROM t_ttl FORCE INDEX (kb)) sk;
