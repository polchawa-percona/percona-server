# Sets $cp_log_count to the number of lines in the server error log that
# contain $cp_log_pattern.
--let CP_LOG_PATTERN = $cp_log_pattern
--let CP_LOG_OUT = $MYSQLTEST_VARDIR/tmp/compaction_partition_log_count.inc
--perl
  my $f = "$ENV{MYSQLTEST_VARDIR}/log/mysqld.1.err";
  my $n = 0;
  if (open(my $fh, '<', $f)) {
    while (<$fh>) { $n++ if index($_, $ENV{CP_LOG_PATTERN}) >= 0; }
    close($fh);
  }
  open(my $out, '>', $ENV{CP_LOG_OUT}) or die "cannot write $ENV{CP_LOG_OUT}";
  print $out "--let \$cp_log_count = $n\n";
  close($out);
EOF
--source $MYSQLTEST_VARDIR/tmp/compaction_partition_log_count.inc
--remove_file $MYSQLTEST_VARDIR/tmp/compaction_partition_log_count.inc
