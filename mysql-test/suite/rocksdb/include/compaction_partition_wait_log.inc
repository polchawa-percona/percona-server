# Waits (up to 120 s) until the server error log has more than $cp_log_count
# lines that contain $cp_log_pattern.
--let CP_LOG_PATTERN = $cp_log_pattern
--let CP_LOG_MIN = $cp_log_count
--perl
  my $f = "$ENV{MYSQLTEST_VARDIR}/log/mysqld.1.err";
  for (my $i = 0; $i < 1200; $i++) {
    my $n = 0;
    if (open(my $fh, '<', $f)) {
      while (<$fh>) { $n++ if index($_, $ENV{CP_LOG_PATTERN}) >= 0; }
      close($fh);
    }
    exit 0 if $n > $ENV{CP_LOG_MIN};
    select(undef, undef, undef, 0.1);
  }
  die "Timed out waiting for '$ENV{CP_LOG_PATTERN}' in the error log";
EOF
