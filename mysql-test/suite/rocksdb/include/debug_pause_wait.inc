# Waits until the pause hook $debug_pause_hook has paused (again) since
# debug_pause_mark.inc was sourced for it, that is until the error log has a
# new "MyRocks: test: paused at <hook>" line, and removes the mark. The
# thread then stays paused until the hook's global debug keyword is removed.
#
# Parameters:
#   $debug_pause_hook     the hook keyword
#   $debug_pause_details  optional: count only pauses whose details start
#                         with this text, e.g. "cf id 3"
#   $debug_pause_log      error log to read (default: server 1's error log)
#   $debug_pause_timeout  seconds to wait (default 60), then the test dies

if (!$debug_pause_hook) {
  --die debug_pause_wait.inc: $debug_pause_hook is not set
}
--let _DPH_HOOK = $debug_pause_hook
--let _DPH_DETAILS = $debug_pause_details
--let _DPH_LOG = $debug_pause_log
if (!$debug_pause_log) {
  --let _DPH_LOG = $MYSQLTEST_VARDIR/log/mysqld.1.err
}
--let _DPH_MARK = $MYSQLTEST_VARDIR/tmp/debug_pause_$debug_pause_hook.mark
--let _DPH_TIMEOUT = $debug_pause_timeout
if (!$debug_pause_timeout) {
  --let _DPH_TIMEOUT = 60
}

--perl
  use strict;
  use Time::HiRes qw(sleep);
  my $hook = $ENV{_DPH_HOOK};
  my $re = ($ENV{_DPH_DETAILS} // '') eq ''
    ? qr/MyRocks: test: paused at \Q$hook\E\b/
    : qr/MyRocks: test: paused at \Q$hook, $ENV{_DPH_DETAILS}\E\b/;
  open(my $m, '<', $ENV{_DPH_MARK})
    or die "debug_pause_wait.inc: source debug_pause_mark.inc first: $!";
  my $mark = <$m>;
  close($m);
  chomp($mark);
  my $tries = $ENV{_DPH_TIMEOUT} * 10;
  while (1) {
    my $n = 0;
    if (open(my $f, '<', $ENV{_DPH_LOG})) {
      while (<$f>) { $n++ if $_ =~ $re; }
      close($f);
    }
    last if $n > $mark;
    die "debug_pause_wait.inc: $hook did not pause in " .
        "$ENV{_DPH_TIMEOUT} seconds" if --$tries <= 0;
    sleep(0.1);
  }
  unlink($ENV{_DPH_MARK});
EOF
