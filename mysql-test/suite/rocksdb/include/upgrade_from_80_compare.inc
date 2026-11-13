# Compares upgcheck.actual with upgcheck.expected for schema $compare_db
# (see std_data/upgrade80_rocksdb/upgrade_80_checks.sql) and prints every
# index whose row count or checksum differs. An empty list is the expected
# result.

--echo # Entries that differ between upgcheck.expected and upgcheck.actual:
--disable_query_log
--eval SELECT e.tbl, e.idx, e.cnt AS expected_cnt, a.cnt AS actual_cnt, e.crc = a.crc AS same_crc FROM upgcheck.expected e LEFT JOIN upgcheck.actual a USING (db, tbl, idx) WHERE e.db = '$compare_db' AND NOT (e.cnt <=> a.cnt AND e.crc <=> a.crc) UNION ALL SELECT a.tbl, a.idx, NULL, a.cnt, NULL FROM upgcheck.actual a LEFT JOIN upgcheck.expected e USING (db, tbl, idx) WHERE a.db = '$compare_db' AND e.db IS NULL ORDER BY 1, 2
--enable_query_log
