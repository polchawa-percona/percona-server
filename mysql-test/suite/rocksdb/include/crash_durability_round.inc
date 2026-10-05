# One round of the crash durability matrix.
#
# Commits $rows two-table transactions (t1 + t2, t1 has a secondary
# index) with rocksdb_flush_log_at_trx_commit = $flush_val, kills the
# server with SIGKILL right after the last commit is acknowledged,
# restarts it and checks what survived.
#
# Parameters:
#   $flush_val   value of rocksdb_flush_log_at_trx_commit for the round
#   $rows        number of transactions to commit
#   $expect_all  1 if every acknowledged commit must survive a process kill
#   $check_binlog 1 if the binary log is enabled and must agree with the
#                engine (needs sync_binlog = 1)
#
# Invariants checked in every round, regardless of $flush_val:
#   - transactions are atomic: t1 and t2 contain the same ids
#   - committed transactions survive in commit order: no holes in ids
#   - the secondary index agrees with the primary key
#   - with the binary log, the engine never has a transaction that the
#     binary log does not have

--echo # Round: rocksdb_flush_log_at_trx_commit = $flush_val
TRUNCATE TABLE t1;
TRUNCATE TABLE t2;
if ($check_binlog)
{
  RESET BINARY LOGS AND GTIDS;
}
--eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $flush_val

--disable_query_log
--let $i = 1
while ($i <= $rows)
{
  BEGIN;
  --eval INSERT INTO t1 VALUES ($i, $i, REPEAT('x', 64))
  --eval INSERT INTO t2 VALUES ($i, $i)
  COMMIT;
  --inc $i
}
--enable_query_log

--source include/kill_and_restart_mysqld.inc

# The restart resets the global value; every check below is on the
# recovered data only.
--let $engine_rows = `SELECT COUNT(*) FROM t1`

SELECT COUNT(*) AS t1_without_t2 FROM t1 LEFT JOIN t2 USING (id)
  WHERE t2.id IS NULL;
SELECT COUNT(*) AS t2_without_t1 FROM t2 LEFT JOIN t1 USING (id)
  WHERE t1.id IS NULL;
SELECT COUNT(*) = COALESCE(MAX(id), 0) AS no_holes FROM t1;
SELECT (SELECT COUNT(*) FROM t1 FORCE INDEX (k) WHERE k > 0) =
       (SELECT COUNT(*) FROM t1 FORCE INDEX (PRIMARY)) AS sk_matches_pk;
SELECT COUNT(*) AS k_mismatch FROM t1 WHERE k <> id;

if ($expect_all)
{
  --replace_result $rows ROWS
  --eval SELECT COUNT(*) = $rows AS all_acknowledged_commits_survived FROM t1
}

if ($check_binlog)
{
  # Count the transactions (Xid events) the binary log holds after
  # recovery.
  --let BINLOG_INDEX = `SELECT @@log_bin_index`
  --let XID_COUNT_FILE = $MYSQLTEST_VARDIR/tmp/crash_durability_xids.inc
  --perl
    use strict;
    my $index = $ENV{'BINLOG_INDEX'};
    my $mysqlbinlog = $ENV{'MYSQL_BINLOG'};
    open(my $ih, '<', $index) or die "open $index: $!";
    my $dir = $index; $dir =~ s{[^/]+$}{};
    my $xids = 0;
    while (my $f = <$ih>) {
      chomp $f;
      $f =~ s{^\./}{};
      $f = $dir . $f unless $f =~ m{^/};
      my $out = `$mysqlbinlog $f 2>/dev/null`;
      $xids++ while $out =~ /\bXid = \d+/g;
    }
    close $ih;
    open(my $oh, '>', $ENV{'XID_COUNT_FILE'}) or die;
    print $oh "--let \$binlog_txns = $xids\n";
    close $oh;
  EOF
  --source $XID_COUNT_FILE
  --remove_file $XID_COUNT_FILE

  --let $engine_not_ahead = `SELECT $engine_rows <= $binlog_txns`
  --echo engine_not_ahead_of_binlog: $engine_not_ahead
  if ($expect_all)
  {
    --let $binlog_has_all = `SELECT $binlog_txns = $rows`
    --echo binlog_has_all_commits: $binlog_has_all
  }
}
