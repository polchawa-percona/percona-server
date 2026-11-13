# One round of crash_2pc_release: start concurrent load, SIGKILL the server
# while the load runs, restart it and check the recovered state.
#
# Parameters:
#   $flush_val    value of rocksdb_flush_log_at_trx_commit for the round
#                 (1 or 2: with the binary log on, both must keep every
#                 acknowledged commit across a process kill)
#   $round        round number, used to vary the client seeds
#   $load_script  path of the load generator written by the test

--echo # Round $round: rocksdb_flush_log_at_trx_commit = $flush_val

--let $_c2_clients = 4
--let $_c2_accounts = 20

TRUNCATE TABLE r_acc;
TRUNCATE TABLE r_log;
TRUNCATE TABLE i_acc;
TRUNCATE TABLE i_log;
--disable_query_log
--let $_c2_a = 1
while ($_c2_a <= $_c2_accounts)
{
  --eval INSERT INTO r_acc VALUES ($_c2_a, 0, 0)
  --eval INSERT INTO i_acc VALUES ($_c2_a, 0, 0)
  --inc $_c2_a
}
--enable_query_log
RESET BINARY LOGS AND GTIDS;
--eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $flush_val
SET GLOBAL innodb_flush_log_at_trx_commit = 1;

--remove_files_wildcard $MYSQLTEST_VARDIR/tmp crash2pc_*

# Start the clients.
--let $_c2_c = 1
while ($_c2_c <= $_c2_clients)
{
  --let $_c2_seed = `SELECT $round * 100 + $_c2_c`
  --exec_in_background bash $load_script "$MYSQL" $_c2_c $_c2_seed $_c2_accounts $MYSQLTEST_VARDIR/tmp/crash2pc_ack_$_c2_c $MYSQLTEST_VARDIR/tmp/crash2pc_done_$_c2_c
  --inc $_c2_c
}

# Let the load run for a while, then kill the server in the middle of it.
--let $wait_timeout = 300
--let $wait_condition = SELECT COUNT(*) >= 200 FROM r_log
--source include/wait_condition.inc
--sleep 1
--source include/kill_mysqld.inc

# Wait until every client has noticed that the server is gone, so that no
# client reconnects to the restarted server.
--perl
  my $dir = "$ENV{MYSQLTEST_VARDIR}/tmp";
  for (1 .. 3000) {
    my $done = grep { -e "$dir/crash2pc_done_$_" } 1 .. 4;
    exit 0 if $done == 4;
    select(undef, undef, undef, 0.1);
  }
  die "load generators did not stop";
EOF

--source include/start_mysqld.inc

# Collect the acknowledged transactions and the transactions committed in the
# binary log, and load them into helper tables.
--let DATADIR = `SELECT @@datadir`
--let BINLOG_INDEX = `SELECT @@log_bin_index`
--let CHK_FILE = $MYSQLTEST_VARDIR/tmp/crash2pc_chk.inc
--perl
  use strict;
  my $tmp = "$ENV{MYSQLTEST_VARDIR}/tmp";
  my $datadir = $ENV{DATADIR};
  my $index = $ENV{BINLOG_INDEX};
  $index = "$datadir/$index" unless $index =~ m{^/};
  my $mysqlbinlog = $ENV{MYSQL_BINLOG};

  my @acked;
  for my $c (1 .. 4) {
    my $f = "$tmp/crash2pc_ack_$c";
    next unless -e $f;
    open(my $fh, '<', $f) or die "open $f: $!";
    while (my $l = <$fh>) { chomp $l; push @acked, $l if $l =~ /^\d+$/; }
    close $fh;
  }

  # Only rows of transactions that reached their Xid event are committed;
  # a transaction cut off by the kill has no Xid and is ignored.
  my (%bl_r, %bl_i, @pend);
  open(my $ih, '<', $index) or die "open $index: $!";
  my $bdir = $index; $bdir =~ s{[^/]+$}{};
  while (my $f = <$ih>) {
    chomp $f;
    $f =~ s{^\./}{};
    $f = $bdir . $f unless $f =~ m{^/};
    my $tbl = '';
    for my $l (`$mysqlbinlog --base64-output=DECODE-ROWS -v $f 2>/dev/null`) {
      if ($l =~ /^BEGIN\s*$/) { @pend = (); next; }
      if ($l =~ /^### INSERT INTO `test`\.`(r_log|i_log)`/) { $tbl = $1; next; }
      if ($l =~ /^### (INSERT|UPDATE|DELETE)/) { $tbl = ''; next; }
      if ($tbl ne '' && $l =~ /^###   \@1=(\d+)/) { push @pend, [$tbl, $1]; $tbl = ''; next; }
      if ($l =~ /\bXid = \d+/) {
        for my $p (@pend) {
          if ($p->[0] eq 'r_log') { $bl_r{$p->[1]} = 1 } else { $bl_i{$p->[1]} = 1 }
        }
        @pend = ();
      }
    }
  }
  close $ih;

  open(my $oh, '>', $ENV{CHK_FILE}) or die "open $ENV{CHK_FILE}: $!";
  print $oh "--disable_query_log\n";
  my %sets = (chk_ack => \@acked, chk_br => [keys %bl_r], chk_bi => [keys %bl_i]);
  for my $t (sort keys %sets) {
    my @ids = @{ $sets{$t} };
    while (my @batch = splice(@ids, 0, 500)) {
      print $oh "INSERT INTO $t VALUES (" . join('),(', @batch) . ");\n";
    }
  }
  print $oh "--enable_query_log\n";
  close $oh;
EOF

CREATE TABLE chk_ack (id INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE chk_br (id INT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE chk_bi (id INT PRIMARY KEY) ENGINE=InnoDB;
--source $CHK_FILE
--remove_file $CHK_FILE

# Atomicity within each engine.
SELECT SUM(bal) = 0 AS rocksdb_zero_sum,
       SUM(moves) = 2 * (SELECT COUNT(*) FROM r_log) AS rocksdb_moves_match_log
  FROM r_acc;
SELECT SUM(bal) = 0 AS innodb_zero_sum,
       SUM(moves) = 2 * (SELECT COUNT(*) FROM i_log) AS innodb_moves_match_log
  FROM i_acc;

# The MyRocks secondary index agrees with the primary key.
SELECT (SELECT GROUP_CONCAT(id, ':', bal ORDER BY id) FROM r_acc FORCE INDEX (kbal)
          WHERE bal > -1000000) =
       (SELECT GROUP_CONCAT(id, ':', bal ORDER BY id) FROM r_acc FORCE INDEX (PRIMARY))
  AS sk_matches_pk;
CHECK TABLE r_acc, r_log;

# Mixed transactions (even ids) are in both engines or in neither.
SELECT COUNT(*) AS mixed_only_in_rocksdb FROM r_log
  WHERE id % 2 = 0 AND id NOT IN (SELECT id FROM i_log);
SELECT COUNT(*) AS mixed_only_in_innodb FROM i_log
  WHERE id NOT IN (SELECT id FROM r_log);

# The load really ran, and every acknowledged commit survived.
SELECT COUNT(*) >= 100 AS load_ran FROM chk_ack;
SELECT COUNT(*) AS acked_lost_in_rocksdb FROM chk_ack
  WHERE id NOT IN (SELECT id FROM r_log);
SELECT COUNT(*) AS acked_lost_in_innodb FROM chk_ack
  WHERE id % 2 = 0 AND id NOT IN (SELECT id FROM i_log);

# The engines hold exactly the transactions committed in the binary log.
SELECT COUNT(*) AS rocksdb_not_in_binlog FROM r_log
  WHERE id NOT IN (SELECT id FROM chk_br);
SELECT COUNT(*) AS binlog_not_in_rocksdb FROM chk_br
  WHERE id NOT IN (SELECT id FROM r_log);
SELECT COUNT(*) AS innodb_not_in_binlog FROM i_log
  WHERE id NOT IN (SELECT id FROM chk_bi);
SELECT COUNT(*) AS binlog_not_in_innodb FROM chk_bi
  WHERE id NOT IN (SELECT id FROM i_log);

DROP TABLE chk_ack, chk_br, chk_bi;
--remove_files_wildcard $MYSQLTEST_VARDIR/tmp crash2pc_*
