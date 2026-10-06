#
# Checks that a failed or killed DDL statement left table t of
# ddl_failure_cleanup_matrix.test unchanged and left nothing behind in the
# MyRocks dictionary: same definition, same rows in the primary key and in
# the secondary index, the same set of indexes in ROCKSDB_DDL, no temporary
# #sql table, and CHECK TABLE passes.
#
SHOW CREATE TABLE t;
SELECT COUNT(*), SUM(a), SUM(b), SUM(LENGTH(c)) FROM t;
SELECT COUNT(*), SUM(LENGTH(c)) FROM t FORCE INDEX (kc) WHERE c >= '';
SELECT INDEX_NAME FROM information_schema.rocksdb_ddl
  WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = 't' ORDER BY INDEX_NAME;
SELECT COUNT(*) AS sql_leftovers FROM information_schema.rocksdb_ddl
  WHERE TABLE_NAME LIKE '#sql%';
CHECK TABLE t;
