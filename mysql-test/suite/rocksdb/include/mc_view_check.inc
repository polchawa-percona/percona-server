# Compares what the open snapshot connections see now with what they saw
# when they were opened (row N of mc.snaps, N = 1..$mc_nsnap) and with the current
# state, and prints one line per connection. At REPEATABLE READ each must
# still see its own version; at READ COMMITTED each must see the latest.
connection default;
--source suite/rocksdb/include/mc_digest.inc
--let $mc_now = $mc_digest
--let $mc_i = 1
while ($mc_i <= $mc_nsnap) {
  connection default;
  --let $mc_then = `SELECT d FROM mc.snaps WHERE n = $mc_i`
  connection con_s$mc_i;
  --source suite/rocksdb/include/mc_digest.inc
  --let $mc_ok_then = `SELECT '$mc_digest' = '$mc_then'`
  --let $mc_ok_now = `SELECT '$mc_digest' = '$mc_now'`
  --echo con_s$mc_i sees its version: $mc_ok_then, sees the latest: $mc_ok_now
  --inc $mc_i
}
connection default;
