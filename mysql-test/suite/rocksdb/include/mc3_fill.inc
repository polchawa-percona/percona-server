# Inserts $mc3_rows rows with keys starting after $mc3_off into $mc3_t of
# shape $mc3_shape.
if (!$mc3_off) {
  --let $mc3_off = 0
}
--disable_query_log
SET SESSION cte_max_recursion_depth = 100000;
if ($mc3_shape == live) {
  eval INSERT INTO $mc3_t
    WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s
                            WHERE n < $mc3_rows)
    SELECT n + $mc3_off, CONCAT('Äb ', (n + $mc3_off) % 37), n + $mc3_off,
           REPEAT(CHAR(65 + n % 26), 100 + n % 300) FROM s;
}
if ($mc3_shape == hid) {
  eval INSERT INTO $mc3_t
    WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s
                            WHERE n < $mc3_rows)
    SELECT n + $mc3_off, CONCAT('Cc', (n + $mc3_off) % 11) FROM s;
}
if ($mc3_shape == pidx) {
  eval INSERT INTO $mc3_t
    WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s
                            WHERE n < $mc3_rows)
    SELECT CONCAT('g', (n + $mc3_off) % 7), n + $mc3_off, (n * 7) % 101,
           'x' FROM s;
}
--enable_query_log
