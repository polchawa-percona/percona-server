# Report the state of MyRocks and of the tables named in $check_tables after a
# DDL crash and restart, in a form that does not depend on the error log.
#
# Usage:
#   --let $check_tables = 't1','t2'      (quoted, comma separated, schema test)
#   --source suite/rocksdb/include/ddl_crash_check.inc
#
# Prints:
#   - whether the ROCKSDB engine is available;
#   - the tables of schema test that the server's data dictionary lists;
#   - the tables MyRocks has in its dictionary (INFORMATION_SCHEMA.ROCKSDB_DDL),
#     including hidden #sql/#truncate_tmp# names, one row per table;
#   - for every listed table that exists in both, its row count.

SELECT COUNT(*) AS rocksdb_available FROM information_schema.ENGINES
  WHERE ENGINE = 'ROCKSDB' AND SUPPORT IN ('YES', 'DEFAULT');

--let $_rdb_up = `SELECT COUNT(*) FROM information_schema.ENGINES WHERE ENGINE = 'ROCKSDB' AND SUPPORT IN ('YES', 'DEFAULT')`

--echo # Server data dictionary:
--sorted_result
--eval SELECT TABLE_NAME, ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME IN ($check_tables)

if ($_rdb_up)
{
  --echo # MyRocks dictionary:
  --replace_regex /#sql2?-[0-9a-f_-]+/#sql-NAME/
  --sorted_result
  SELECT DISTINCT TABLE_NAME FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = 'test';

  --let $_i = 0
  --let $_t = `SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME IN ($check_tables) ORDER BY TABLE_NAME LIMIT $_i, 1`
  while ($_t)
  {
    --let $_in_rdb = `SELECT COUNT(*) FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$_t'`
    if ($_in_rdb)
    {
      --error 0,1815
      --eval SELECT '$_t' AS table_name, COUNT(*) AS row_count FROM test.$_t
      --error 0,1815
      --eval CHECK TABLE test.$_t
    }
    if (!$_in_rdb)
    {
      --echo # $_t is in the server data dictionary but not in MyRocks
    }
    --inc $_i
    --let $_t = `SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME IN ($check_tables) ORDER BY TABLE_NAME LIMIT $_i, 1`
  }
}
