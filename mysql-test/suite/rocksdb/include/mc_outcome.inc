# Prints the error number of the previous statement (0 on success), for
# statements run with --error 0,ER_LOCK_DEADLOCK,... where the expected
# outcome depends on the isolation level.
--echo $mc_what: $mysql_errno
