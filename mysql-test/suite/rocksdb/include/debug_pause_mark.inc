# Records how many times the pause hook $debug_pause_hook has paused so far,
# so that debug_pause_wait.inc can wait for its next pause. Source it before
# the hook's global debug keyword is set. The hooks are listed in
# storage/rocksdb/rdb_utils.h.
#
# Parameters:
#   $debug_pause_hook  the hook keyword, e.g. rocksdb_pause_drop_index_thread
#   $debug_pause_details  optional: count only pauses whose details start
#                         with this text, e.g. "cf id 3"
#   $debug_pause_log   error log to read (default: server 1's error log)

if (!$debug_pause_hook) {
  --die debug_pause_mark.inc: $debug_pause_hook is not set
}
--let _DPH_HOOK = $debug_pause_hook
--let _DPH_DETAILS = $debug_pause_details
--let _DPH_LOG = $debug_pause_log
if (!$debug_pause_log) {
  --let _DPH_LOG = $MYSQLTEST_VARDIR/log/mysqld.1.err
}
--let _DPH_MARK = $MYSQLTEST_VARDIR/tmp/debug_pause_$debug_pause_hook.mark

--perl
  use strict;
  my $hook = $ENV{_DPH_HOOK};
  my $re = ($ENV{_DPH_DETAILS} // '') eq ''
    ? qr/MyRocks: test: paused at \Q$hook\E\b/
    : qr/MyRocks: test: paused at \Q$hook, $ENV{_DPH_DETAILS}\E\b/;
  my $n = 0;
  if (open(my $f, '<', $ENV{_DPH_LOG})) {
    while (<$f>) { $n++ if $_ =~ $re; }
    close($f);
  }
  open(my $m, '>', $ENV{_DPH_MARK}) or die "cannot write mark: $!";
  print $m "$n\n";
  close($m);
EOF
