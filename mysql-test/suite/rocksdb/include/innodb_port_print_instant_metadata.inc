# MyRocks replacement for suite/innodb/include/print_instant_metadata.inc.
#
# InnoDB reports instant metadata in INFORMATION_SCHEMA.INNODB_TABLES and
# INNODB_COLUMNS. MyRocks keeps it only in the data dictionary:
#   - mysql.tables.se_private_data: instant_col=<number of columns the table
#     had before its first instant ADD COLUMN>;
#   - mysql.columns.se_private_data: default=<hex> or default_null=1 for each
#     instantly added column (the value that rows written before the ADD
#     COLUMN return).
# Both are cleared when the table is rebuilt or truncated.
#
# Needs a debug build (skip_dd_table_access_check). Set $table_name.

--disable_query_log
SET SESSION debug = '+d,skip_dd_table_access_check';
--eval SELECT ID INTO @ipim_tid FROM mysql.tables WHERE NAME = '$table_name' AND SCHEMA_ID = (SELECT ID FROM mysql.schemata WHERE NAME = DATABASE())
--echo # DD metadata of table $table_name
SELECT NAME, SE_PRIVATE_DATA FROM mysql.tables WHERE ID = @ipim_tid;
--echo # DD metadata of the columns of $table_name
SELECT NAME, ORDINAL_POSITION, HIDDEN, SE_PRIVATE_DATA FROM mysql.columns
  WHERE TABLE_ID = @ipim_tid AND HIDDEN <> 'SE' ORDER BY ORDINAL_POSITION;
SET SESSION debug = '-d,skip_dd_table_access_check';
--enable_query_log
