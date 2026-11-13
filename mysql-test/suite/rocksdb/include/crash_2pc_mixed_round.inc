# One crash point of the mixed-engine 2PC test.
#
# Parameters:
#   $point     DBUG keyword of the crash point in the commit path
#   $id        id of the transaction (one row in ti and one in tr)
#   $expected  1 if the transaction must be committed after recovery,
#              0 if it must be rolled back

--echo # Crash point: $point
BEGIN;
--eval INSERT INTO ti VALUES ($id, 'innodb')
--eval INSERT INTO tr VALUES ($id, 'rocksdb')
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--eval SET SESSION debug = "+d,$point"
--error CR_SERVER_LOST
COMMIT;
--source include/wait_until_connected_again.inc

--eval SELECT (SELECT COUNT(*) FROM ti WHERE id = $id) AS innodb_has_row, (SELECT COUNT(*) FROM tr WHERE id = $id) AS rocksdb_has_row, $expected AS expected
--let $agree = `SELECT (SELECT COUNT(*) FROM ti WHERE id = $id) = (SELECT COUNT(*) FROM tr WHERE id = $id)`
if (!$agree)
{
  --echo ENGINES DISAGREE after crash at $point
}
