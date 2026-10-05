# Checks table t1 (c, id) with indexes kc (c), kp (c(3)), kid (id) and
# PRIMARY KEY (c, id): every row is found through each index, and the index
# scans see the same rows as a scan by kid.
SELECT COUNT(*) AS n,
  SUM(CRC32(HEX(c))) AS crc_kid FROM t1 FORCE INDEX (kid);
SELECT (SELECT SUM(CRC32(HEX(c))) FROM t1 FORCE INDEX (kc)) AS crc_kc,
  (SELECT SUM(CRC32(HEX(c))) FROM t1 FORCE INDEX (kp)) AS crc_kp,
  (SELECT SUM(CRC32(HEX(c))) FROM t1 FORCE INDEX (PRIMARY)) AS crc_pk;
--echo # Rows not found by an equality lookup on c through kc, kp and PRIMARY:
SELECT
  (SELECT COUNT(*) FROM t1 AS x WHERE NOT EXISTS
    (SELECT 1 FROM t1 AS y FORCE INDEX (kc) WHERE y.c = x.c AND y.id = x.id)) AS miss_kc,
  (SELECT COUNT(*) FROM t1 AS x WHERE NOT EXISTS
    (SELECT 1 FROM t1 AS y FORCE INDEX (kp) WHERE y.c = x.c AND y.id = x.id)) AS miss_kp,
  (SELECT COUNT(*) FROM t1 AS x WHERE NOT EXISTS
    (SELECT 1 FROM t1 AS y FORCE INDEX (PRIMARY) WHERE y.c = x.c AND y.id = x.id)) AS miss_pk;
--echo # Number of rows equal to each value under the new collation, by index:
SELECT x.id, (SELECT COUNT(*) FROM t1 AS y FORCE INDEX (kc) WHERE y.c = x.c) AS eq_kc,
  (SELECT COUNT(*) FROM t1 AS y IGNORE INDEX (kc, kp, PRIMARY) WHERE y.c = x.c) AS eq_scan
  FROM t1 AS x FORCE INDEX (kid) ORDER BY x.id;
CHECK TABLE t1;
