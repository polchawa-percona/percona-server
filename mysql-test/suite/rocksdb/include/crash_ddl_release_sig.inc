# Sets $ddl_sig to a signature of table $ddl_sig_table (default t1): its columns,
# indexes and partitions, without the table name. Used by crash_ddl_release (R6).
if (!$ddl_sig_table)
{
  --let $ddl_sig_table = t1
}
--disable_query_log
let $ddl_sig = `SELECT CONCAT(
  (SELECT GROUP_CONCAT(CONCAT(COLUMN_NAME, ':', COLUMN_TYPE, ':', IS_NULLABLE, ':', IFNULL(COLUMN_DEFAULT, '~')) ORDER BY ORDINAL_POSITION)
     FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$ddl_sig_table'),
  '|',
  IFNULL((SELECT GROUP_CONCAT(CONCAT(INDEX_NAME, ':', NON_UNIQUE, ':', COLUMN_NAME, ':', SEQ_IN_INDEX) ORDER BY INDEX_NAME, SEQ_IN_INDEX)
     FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$ddl_sig_table'), ''),
  '|',
  IFNULL((SELECT GROUP_CONCAT(PARTITION_NAME ORDER BY PARTITION_ORDINAL_POSITION)
     FROM information_schema.PARTITIONS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$ddl_sig_table'), ''))`;
--enable_query_log
--let $ddl_sig_table =
