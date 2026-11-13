# Body of rocksdb.partition_vgcol_blob_ordered_scan, run once per engine.
# Parameter: $engine

eval CREATE TABLE t1 (
  ip_col INT, b1 BLOB, i2 INT NOT NULL, f3 FLOAT,
  g4 BLOB GENERATED ALWAYS AS (CONCAT(SUBSTR(ip_col,1,5), REPEAT('x', 40))) VIRTUAL,
  KEY k_b1 (b1(12)), KEY k_i2_g4 (i2, g4(22))
) ENGINE=$engine PARTITION BY KEY (ip_col) PARTITIONS 23;

# i2 is not unique (50 rows per value, spread over the partitions), so the
# merge of the per-partition streams compares the g4 key part of rows that
# tie on i2.
INSERT INTO t1 (ip_col, b1, i2, f3)
  WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM s WHERE n < 500)
  SELECT n, REPEAT('b', 30), n % 10, n / 3 FROM s;
ANALYZE TABLE t1;

# Ordered index scan over all partitions. Under ASAN this reports
# heap-use-after-free in key_rec_cmp() / Field_blob::cmp_max().
--enable_info
UPDATE t1 FORCE INDEX (k_i2_g4) SET b1 = REPEAT('u', 200) WHERE i2 BETWEEN 2 AND 7;
--disable_info
SELECT COUNT(*) FROM t1 WHERE b1 = REPEAT('u', 200);

# An UPDATE with ORDER BY ... LIMIT must change exactly the first 7 rows in
# (i2, g4) order. g4 is indexed only by a prefix, so the sort is a filesort
# over the rows of the range scan.
--replace_column 10 #
EXPLAIN UPDATE t1 FORCE INDEX (k_i2_g4) SET b1 = REPEAT('v', 200)
  WHERE i2 BETWEEN 2 AND 7 ORDER BY i2, g4 LIMIT 7;
let $expected = `SELECT GROUP_CONCAT(ip_col ORDER BY i2, g4) FROM
  (SELECT ip_col, i2, g4 FROM t1 IGNORE INDEX (k_i2_g4, k_b1)
   WHERE i2 BETWEEN 2 AND 7 ORDER BY i2, g4 LIMIT 7) AS d`;
--enable_info
UPDATE t1 FORCE INDEX (k_i2_g4) SET b1 = REPEAT('v', 200)
  WHERE i2 BETWEEN 2 AND 7 ORDER BY i2, g4 LIMIT 7;
--disable_info
SELECT ip_col, i2, g4 FROM t1 WHERE b1 = REPEAT('v', 200) ORDER BY i2, g4;
let $updated = `SELECT GROUP_CONCAT(ip_col ORDER BY i2, g4) FROM t1
  WHERE b1 = REPEAT('v', 200)`;
--let $assert_text = UPDATE ... ORDER BY ... LIMIT 7 changed the first 7 rows
--let $assert_cond = "$updated" = "$expected"
--source include/assert.inc

# The index scan must return the same rows as a scan without the index.
let $via_index = `SELECT CONCAT(COUNT(*), ':', SUM(ip_col), ':', SUM(CRC32(g4)))
  FROM t1 FORCE INDEX (k_i2_g4) WHERE i2 BETWEEN 2 AND 7`;
let $via_scan = `SELECT CONCAT(COUNT(*), ':', SUM(ip_col), ':', SUM(CRC32(g4)))
  FROM t1 IGNORE INDEX (k_i2_g4, k_b1) WHERE i2 BETWEEN 2 AND 7`;
--let $assert_text = Index scan and table scan return the same rows
--let $assert_cond = "$via_index" = "$via_scan"
--source include/assert.inc

SELECT i2, g4 FROM t1 FORCE INDEX (k_i2_g4)
  WHERE i2 BETWEEN 2 AND 3 ORDER BY i2, g4 LIMIT 3;
CHECK TABLE t1;
DROP TABLE t1;
