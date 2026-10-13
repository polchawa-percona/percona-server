# Compares the checksums recorded under $mc3_from and $mc3_to. Prints
# nothing but a header when they match.
--echo # Compare $mc3_from with $mc3_to
--disable_query_log
eval SELECT a.tbl, a.idx, a.cnt AS cnt_from, b.cnt AS cnt_to,
  a.crc = b.crc AS same_crc
  FROM mc3.chk a LEFT JOIN mc3.chk b
    ON b.label = '$mc3_to' AND b.tbl = a.tbl AND b.idx = a.idx
  WHERE a.label = '$mc3_from'
    AND (b.cnt IS NULL OR a.cnt <> b.cnt OR a.crc <> b.crc);
eval SELECT tbl, MIN(cnt) = MAX(cnt) AS all_indexes_agree, MAX(cnt) AS cnt
  FROM mc3.chk WHERE label = '$mc3_to' GROUP BY tbl ORDER BY tbl;
--enable_query_log
