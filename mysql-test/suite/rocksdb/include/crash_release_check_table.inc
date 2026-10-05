# Invariant checks for table test.$crt_table after a crash (release-capable):
#   - the rows in the engine equal the rows the binary log says were
#     committed into the table: rows of an interrupted statement must not
#     stay in the engine, and rows of committed statements must survive;
#   - when $crt_sk is set, secondary index `k` agrees with the primary key
#     and holds no entries for rows the primary key does not have;
#   - CHECK TABLE is OK;
#   - no temporary bulk-load SST files are left behind.
# Only booleans are printed, so the result does not depend on where exactly
# the kill landed.

--let BINLOG_INDEX = `SELECT @@log_bin_index`
--let CRT_OUT = $MYSQLTEST_VARDIR/tmp/crt_binlog_rows.inc
# Count binlog rows per table in test.*; the generated file then picks the
# one for $crt_table.
--perl
  use strict;
  my $index = $ENV{'BINLOG_INDEX'};
  my $mysqlbinlog = $ENV{'MYSQL_BINLOG'};
  open(my $ih, '<', $index) or die "open $index: $!";
  my $dir = $index; $dir =~ s{[^/]+$}{};
  my %rows;
  while (my $f = <$ih>) {
    chomp $f;
    $f =~ s{^\./}{};
    $f = $dir . $f unless $f =~ m{^/};
    # --force-if-open: the current binlog is still open (and the one written
    # before a kill was never closed); without it mysqlbinlog prints nothing.
    open(my $p, '-|', "$mysqlbinlog --force-if-open --base64-output=decode-rows -v $f 2>/dev/null") or die;
    while (my $l = <$p>) {
      $rows{$1}++ if $l =~ /^### INSERT INTO `test`\.`([^`]+)`/;
      $rows{$1}-- if $l =~ /^### DELETE FROM `test`\.`([^`]+)`/;
    }
    close $p;
  }
  close $ih;
  open(my $oh, '>', $ENV{'CRT_OUT'}) or die;
  print $oh "--let \$crt_binlog_rows = 0\n";
  for my $t (sort keys %rows) {
    print $oh "if (\$crt_table == $t)\n{\n  --let \$crt_binlog_rows = $rows{$t}\n}\n";
  }
  close $oh;
EOF
--source $CRT_OUT
--remove_file $CRT_OUT

--let $_crt_pk = `SELECT COUNT(*) FROM $crt_table FORCE INDEX (PRIMARY)`
--echo # $crt_table after recovery
# The row counts depend on where the kill landed, so only booleans are
# printed; the counts go to the log when an invariant fails.
--disable_query_log
--eval SELECT $_crt_pk = $crt_binlog_rows AS engine_matches_binlog
if ($_crt_pk != $crt_binlog_rows)
{
  --exec echo "crash_release_check_table: $crt_table engine rows $_crt_pk, binlog rows $crt_binlog_rows" >&2
}
if ($crt_sk)
{
  --eval SELECT (SELECT COUNT(*) FROM $crt_table FORCE INDEX (k)) = $_crt_pk AS sk_matches_pk
  --eval SELECT COUNT(*) = 0 AS no_sk_entries_without_pk FROM $crt_table FORCE INDEX (k) WHERE id NOT IN (SELECT id FROM $crt_table IGNORE INDEX (k))
}
--enable_query_log
--eval CHECK TABLE $crt_table
--let ROCKSDB_DIR = `SELECT CONCAT(@@datadir, '/', @@rocksdb_datadir)`
--perl
  my @left = glob("$ENV{'ROCKSDB_DIR'}/*.bulk_load.tmp.sst");
  print "leftover_tmp_sst_files: " . scalar(@left) . "\n";
EOF
--let $crt_sk =
