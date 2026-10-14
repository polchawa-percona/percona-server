#
# Scratch (crash campaign): kill the server while $cs_stmt waits at each
# DEBUG_SYNC point it passes (first hit of each point), restart, and check that
# the recovered state is the state before or after the statement, that the
# invariants hold and that the table accepts writes.
#
# $cs_reset, $cs_stmt, $cs_state_proc (optional), $cs_state, $cs_label,
# $cs_dml_check (optional), $cs_skip (optional, comma list of points to skip)
#
--disable_query_log
--disable_result_log
eval $cs_reset;
if ($cs_state_proc) {
  eval $cs_state_proc;
}
--let $cs_before = `$cs_state`
--let $cs_trace = $MYSQLTEST_VARDIR/tmp/cs_trace.log
--remove_files_wildcard $MYSQLTEST_VARDIR/tmp cs_trace.log
eval SET SESSION debug = 'd,debug_sync_point:o,$cs_trace';
eval $cs_stmt;
SET SESSION debug = '';
if ($cs_state_proc) {
  eval $cs_state_proc;
}
--let $cs_after = `$cs_state`
--let CS_TRACE = $cs_trace
--let CS_OUT = $MYSQLTEST_VARDIR/tmp/cs_points.inc
--let CS_SKIP = $cs_skip
perl;
  my %seen; my @p;
  my %skip = map { $_ => 1 } split(/,/, $ENV{CS_SKIP} // '');
  open(my $f, '<', $ENV{CS_TRACE}) or die "no trace";
  while (<$f>) {
    next unless /hit: '([^']+)'/;
    next if $seen{$1}++ || $skip{$1};
    push @p, $1;
  }
  open(my $o, '>', $ENV{CS_OUT}) or die;
  print $o "--let \$cs_points = " . join(',', @p) . "\n";
  print $o "--let \$cs_npoints = " . scalar(@p) . "\n";
EOF
--source $MYSQLTEST_VARDIR/tmp/cs_points.inc
--echo # $cs_label: sync points enumerated
--let $cs_v = 0
--let $cs_p = $cs_points
while ($cs_p) {
  --let $cs_point = `SELECT SUBSTRING_INDEX('$cs_p', ',', 1)`
  --let $cs_p = `SELECT IF(LOCATE(',', '$cs_p') > 0, SUBSTRING('$cs_p', LOCATE(',', '$cs_p') + 1), '')`
  eval $cs_reset;
  connect (cs_con, localhost, root,,test);
  --disable_query_log
  --disable_result_log
  eval SET DEBUG_SYNC = '$cs_point SIGNAL cs_reached WAIT_FOR cs_never';
  --send_eval $cs_stmt
  --connection default
  --let $cs_ok_wait = 1
  --disable_abort_on_error
  SET DEBUG_SYNC = 'now WAIT_FOR cs_reached TIMEOUT 60';
  --enable_abort_on_error
  --source include/expect_crash.inc
  --shutdown_server 0
  --source include/wait_until_disconnected.inc
  --connection cs_con
  --error 0,2013,2006,1
  --reap
  --disconnect cs_con
  --connection default
  --source include/start_mysqld.inc
  --disable_query_log
  --disable_result_log
  if ($cs_state_proc) {
    eval $cs_state_proc;
  }
  --let $cs_now = `$cs_state`
  --let $cs_good = 0
  if ($cs_now == $cs_before) {
    --let $cs_good = 1
  }
  if ($cs_now == $cs_after) {
    --let $cs_good = 1
  }
  if (!$cs_good) {
    --enable_result_log
    --echo VIOLATION $cs_label at $cs_point: recovered state is neither before nor after
    --echo   before:    $cs_before
    --echo   after:     $cs_after
    --echo   recovered: $cs_now
    --disable_result_log
    --inc $cs_v
  }
  --let $inv_schema = test
  --let $inv_drop_timeout = 120
  --source suite/rocksdb/include/myrocks_invariants.inc
  --disable_query_log
  --disable_result_log
  if ($_inv_violations) {
    --echo VIOLATION $cs_label at $cs_point: invariant violation(s)
    --inc $cs_v
  }
  if ($cs_dml_check) {
    --disable_abort_on_error
    eval $cs_dml_check;
    --let $cs_err = $mysql_errno
    --enable_abort_on_error
    if ($cs_err) {
      --echo VIOLATION $cs_label at $cs_point: write after recovery failed with error $cs_err
      --inc $cs_v
    }
  }
}
--exec echo "$cs_label: points=$cs_npoints violations=$cs_v [$cs_points]" >> $MYSQLTEST_VARDIR/log/cs_stats.log
--enable_query_log
--enable_result_log
if (!$cs_v) {
  --echo # $cs_label: every kill recovered correctly
}
