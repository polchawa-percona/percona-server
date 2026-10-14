#
# Waits until the error log holds $errinj_bg_count lines that report an
# error injected by rocksdb_debug_inject_bg_error at site $errinj_bg_site
# (the count includes earlier lines of the same site in this server's log).
# Fails the test after 120 seconds.
#
if (!$errinj_bg_count) {
  --let $errinj_bg_count = 1
}
--let EBGW_SITE = $errinj_bg_site
--let EBGW_COUNT = $errinj_bg_count
--let ERRINJ_BG_LOG = $MYSQLTEST_VARDIR/log/mysqld.1.err
--let ERRINJ_BG_OUT = $MYSQLTEST_VARDIR/tmp/errinj_bg_wait.inc
perl;
  my $site = $ENV{EBGW_SITE};
  my $want = $ENV{EBGW_COUNT};
  my $n = 0;
  for (my $i = 0; $i < 1200; $i++) {
    $n = 0;
    open(my $f, '<', $ENV{ERRINJ_BG_LOG}) or die "cannot open error log";
    while (<$f>) {
      $n++ if index($_, 'MyRocks: injected error ') >= 0 &&
              index($_, " at $site in background thread ") >= 0;
    }
    close($f);
    last if $n >= $want;
    select(undef, undef, undef, 0.1);
  }
  open(my $o, '>', $ENV{ERRINJ_BG_OUT}) or die "cannot write";
  print $o "--let \$errinj_bg_found = $n\n";
  close($o);
EOF
--source $MYSQLTEST_VARDIR/tmp/errinj_bg_wait.inc
--remove_file $MYSQLTEST_VARDIR/tmp/errinj_bg_wait.inc
if ($errinj_bg_found < $errinj_bg_count) {
  --echo injected error at $errinj_bg_site reported $errinj_bg_found times, expected $errinj_bg_count
  --die The injected background error was not reported
}
