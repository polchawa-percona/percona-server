# Restart the server and check that MyRocks found no pending inplace ALTER
# TABLE record (Rdb_key_def::DDL_PENDING_ALTER) at startup.

--source include/restart_mysqld.inc
--let $assert_text = No pending ALTER TABLE record is left
--let $assert_file = $MYSQLTEST_VARDIR/log/mysqld.1.err
--let $assert_select = has an unfinished ALTER TABLE
--let $assert_only_after = starting as process
--let $assert_count = 0
--source include/assert_grep.inc
