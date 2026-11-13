# Assert-style report after a DDL crash and restart: MyRocks is available and
# the server's data dictionary and the MyRocks dictionary list the same tables
# among $check_tables (schema test).
#
# Usage:
#   --let $check_tables = 't1','t2'
#   --source suite/rocksdb/include/ddl_crash_consistent.inc

SELECT COUNT(*) AS rocksdb_available FROM information_schema.ENGINES
  WHERE ENGINE = 'ROCKSDB' AND SUPPORT IN ('YES', 'DEFAULT');
--eval SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME IN ($check_tables)) = (SELECT COUNT(DISTINCT TABLE_NAME) FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME IN ($check_tables)) AS dictionaries_agree
