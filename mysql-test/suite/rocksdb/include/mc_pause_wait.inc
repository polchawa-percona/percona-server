# Waits until a manual compaction is held at $mc_point again after the last
# mc_pause_mark.inc.
#
# $mc_point  start, middle or completed

--let PAUSE_POINT_NAME = `SELECT '$mc_point'`
--let MC_MARK_FILE = $MYSQLTEST_VARDIR/tmp/mc_pause_mark.txt
--perl
  my $log = "$ENV{MYSQLTEST_VARDIR}/log/mysqld.1.err";
  open(my $m, '<', $ENV{MC_MARK_FILE}) or die;
  my $before = <$m>;
  close($m);
  chomp $before;
  for (my $i = 0; $i < 3000; $i++) {
    my $n = 0;
    if (open(my $f, '<', $log)) {
      while (<$f>) { $n++ if /manual compaction paused at $ENV{PAUSE_POINT_NAME},/; }
      close($f);
    }
    if ($n > $before) { unlink($ENV{MC_MARK_FILE}); exit 0; }
    select(undef, undef, undef, 0.1);
  }
  die "manual compaction was not held at $ENV{PAUSE_POINT_NAME} within 300 s";
EOF
