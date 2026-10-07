# Records how many times a manual compaction has been held at $mc_point so
# far, so that mc_pause_wait.inc can wait for the next hold. Holds are
# logged to the error log by the rocksdb_mc_pause_<point> debug keywords.
#
# $mc_point  start, middle or completed

--let PAUSE_POINT_NAME = `SELECT '$mc_point'`
--let MC_MARK_FILE = $MYSQLTEST_VARDIR/tmp/mc_pause_mark.txt
--perl
  my $log = "$ENV{MYSQLTEST_VARDIR}/log/mysqld.1.err";
  my $n = 0;
  if (open(my $f, '<', $log)) {
    while (<$f>) { $n++ if /manual compaction paused at $ENV{PAUSE_POINT_NAME},/; }
    close($f);
  }
  open(my $o, '>', $ENV{MC_MARK_FILE}) or die;
  print $o "$n\n";
  close($o);
EOF
