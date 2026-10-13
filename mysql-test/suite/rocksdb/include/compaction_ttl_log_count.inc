# Sets $ctl_count to the number of lines in the server error log that contain
# $ctl_pattern.
--let CTL_PATTERN = $ctl_pattern
--let CTL_OUT = $MYSQLTEST_VARDIR/tmp/compaction_ttl_log_count.inc
--perl
  my $f = "$ENV{MYSQLTEST_VARDIR}/log/mysqld.1.err";
  my $n = 0;
  if (open(my $fh, '<', $f)) {
    while (<$fh>) { $n++ if index($_, $ENV{CTL_PATTERN}) >= 0; }
    close($fh);
  }
  open(my $out, '>', $ENV{CTL_OUT}) or die "cannot write $ENV{CTL_OUT}";
  print $out "--let \$ctl_count = $n\n";
  close($out);
EOF
--source $MYSQLTEST_VARDIR/tmp/compaction_ttl_log_count.inc
--remove_file $MYSQLTEST_VARDIR/tmp/compaction_ttl_log_count.inc
