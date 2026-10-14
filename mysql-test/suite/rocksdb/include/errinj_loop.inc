#
# Systematic error injection into the RocksDB calls made by one statement.
#
# For N = 1, 2, ... the statement is run again from the same starting state
# with rocksdb_debug_inject_error_nth = N, so that the N-th RocksDB call of
# class $errinj_class made by this session fails with $errinj_status instead
# of being executed. The loop ends when the statement makes fewer than N such
# calls (the counter is left above zero), or at $errinj_max.
#
# After every injected failure the statement must either
#   - report an error and leave the starting state ($errinj_state before), or
#   - succeed and leave the same state as an uninjected run.
# A server abort is accepted only with $errinj_crash = 1: the server is then
# restarted and the recovered state must be one of those two states.
# CHECK TABLE must report OK on every table in $errinj_tables in all cases.
#
# Parameters:
#   $errinj_class    ANY, WRITE, READ, ITERATOR, COMMIT, DICT, SST, INGEST,
#                    COMPACT, DICT_READ
#   $errinj_status   IOERROR, NOSPACE, CORRUPTION, BUSY, TIMEDOUT, TRYAGAIN,
#                    INCOMPLETE, ABORTED, MEMORYLIMIT, EXPIRED,
#                    INVALIDARGUMENT
#   $errinj_reset    statement that recreates the starting state
#   $errinj_stmt     the statement under test (use a procedure for several)
#   $errinj_state    query returning one value without spaces that describes
#                    the state (for example per-index counts and checksums)
#   $errinj_state_proc optional statement run before every $errinj_state
#   $errinj_tables   comma-separated tables for CHECK TABLE
#   $errinj_max      upper bound for N (default 200)
#   $errinj_crash    1 if the injected failure may abort the server
#   $errinj_label    text used in the output (default class/status)
#   $errinj_after_restart optional statement run after every restart, for
#                    example to set global variables again
#
# Output: one line per violation, then one summary line without counts, so
# that the result does not depend on how many RocksDB calls are made.
#

if (!$errinj_max) {
  --let $errinj_max = 200
}
if (!$errinj_label) {
  --let $errinj_label = $errinj_class/$errinj_status
}

--disable_query_log
--disable_result_log

# Reference run without injection.
eval $errinj_reset;
--source suite/rocksdb/include/errinj_state_proc.inc
--let $errinj_before = `$errinj_state`
eval $errinj_stmt;
--source suite/rocksdb/include/errinj_state_proc.inc
--let $errinj_after = `$errinj_state`
if ($errinj_before == $errinj_after) {
  --echo # $errinj_label: the statement does not change the state
}

--let $errinj_n = 1
--let $errinj_done = 0
--let $errinj_violations = 0
--let $errinj_fired = 0
--let $errinj_errors = 0
--let $errinj_crashes = 0
while (!$errinj_done) {
  eval $errinj_reset;
  eval SET SESSION rocksdb_debug_inject_error_class = '$errinj_class';
  eval SET SESSION rocksdb_debug_inject_error_status = '$errinj_status';
  if ($errinj_crash) {
    --exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
  }
  eval SET SESSION rocksdb_debug_inject_error_nth = $errinj_n;

  --disable_abort_on_error
  eval $errinj_stmt;
  --let $errinj_err = $mysql_errno
  --enable_abort_on_error

  --let $errinj_crashed = 0
  if ($errinj_err == 2013) {
    --let $errinj_crashed = 1
  }
  if ($errinj_err == 2006) {
    --let $errinj_crashed = 1
  }

  if ($errinj_crashed) {
    if (!$errinj_crash) {
      --echo VIOLATION $errinj_label n=$errinj_n: server aborted
      --die The server aborted on an injected error that must not abort it
    }
    --inc $errinj_crashes
    --inc $errinj_fired
    --source include/wait_until_disconnected.inc
    --exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
    --source include/wait_until_connected_again.inc
    # The include files above turn the query and result logs back on.
    --disable_query_log
    --disable_result_log
    # MyRocks may refuse to start when the dictionaries disagree; restart
    # without table validation so that the loop can go on.
    --let $errinj_engine = `SELECT COUNT(*) FROM information_schema.engines WHERE engine = 'ROCKSDB' AND support IN ('YES', 'DEFAULT')`
    if (!$errinj_engine) {
      --echo VIOLATION $errinj_label n=$errinj_n: MyRocks did not start after the abort
      --inc $errinj_violations
      --let $restart_parameters = restart: --rocksdb_validate_tables=2
      --source include/restart_mysqld.inc
      --let $restart_parameters =
      --disable_query_log
      --disable_result_log
    }
    # Global settings are lost on restart.
    if ($errinj_after_restart) {
      eval $errinj_after_restart;
    }
    --source suite/rocksdb/include/errinj_state_proc.inc
    --let $errinj_state_now = `$errinj_state`
    --let $errinj_ok = 0
    if ($errinj_state_now == $errinj_before) {
      --let $errinj_ok = 1
    }
    if ($errinj_state_now == $errinj_after) {
      --let $errinj_ok = 1
    }
    if (!$errinj_ok) {
      --echo VIOLATION $errinj_label n=$errinj_n: recovered state is neither the starting nor the final state
      --echo   before:    $errinj_before
      --echo   after:     $errinj_after
      --echo   recovered: $errinj_state_now
      --inc $errinj_violations
    }
  }

  if (!$errinj_crashed) {
    if ($errinj_crash) {
      --remove_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
    }
    ROLLBACK;
    --let $errinj_left = `SELECT @@session.rocksdb_debug_inject_error_nth`
    SET SESSION rocksdb_debug_inject_error_nth = 0;
    if ($errinj_left) {
      # Fewer than N calls: nothing was injected, the loop is complete.
      --let $errinj_done = 1
      if ($errinj_err) {
        --echo VIOLATION $errinj_label n=$errinj_n: error $errinj_err without an injected failure
        --inc $errinj_violations
      }
    }
    if (!$errinj_left) {
      --inc $errinj_fired
      --source suite/rocksdb/include/errinj_state_proc.inc
    --let $errinj_state_now = `$errinj_state`
      if ($errinj_err) {
        --inc $errinj_errors
        if ($errinj_state_now != $errinj_before) {
          --echo VIOLATION $errinj_label n=$errinj_n: error $errinj_err but the state changed
          --echo   before: $errinj_before
          --echo   now:    $errinj_state_now
          --inc $errinj_violations
        }
      }
      if (!$errinj_err) {
        if ($errinj_state_now != $errinj_after) {
          --echo VIOLATION $errinj_label n=$errinj_n: success reported but the state differs from an uninjected run
          --echo   expected: $errinj_after
          --echo   now:      $errinj_state_now
          --inc $errinj_violations
        }
      }
    }
  }

  if (!$errinj_done) {
    # Every table must stay internally consistent.
    --let $errinj_t = $errinj_tables
    while ($errinj_t) {
      --let $errinj_one = `SELECT SUBSTRING_INDEX('$errinj_t', ',', 1)`
      --let $errinj_t = `SELECT IF(LOCATE(',', '$errinj_t') > 0, SUBSTRING('$errinj_t', LOCATE(',', '$errinj_t') + 1), '')`
      --let $errinj_chk = query_get_value(CHECK TABLE $errinj_one, Msg_text, 1)
      if ($errinj_chk != OK) {
        --echo VIOLATION $errinj_label n=$errinj_n: CHECK TABLE $errinj_one: $errinj_chk
        --inc $errinj_violations
      }
    }
  }

  --inc $errinj_n
  if ($errinj_n > $errinj_max) {
    --echo # $errinj_label: stopped at the loop bound $errinj_max
    --let $errinj_done = 1
  }
}

# Debugging aid, not part of the result: how the injected failures ended.
--exec echo "$errinj_label: injected=$errinj_fired errors=$errinj_errors crashes=$errinj_crashes violations=$errinj_violations" >> $MYSQLTEST_VARDIR/log/errinj_stats.log
--enable_result_log
--enable_query_log

if (!$errinj_fired) {
  --echo # $errinj_label: no RocksDB call of this class was made
}
if ($errinj_fired) {
  if (!$errinj_violations) {
    --echo # $errinj_label: every injected failure was handled correctly
  }
  if ($errinj_violations) {
    --echo # $errinj_label: violations found
  }
}

--let $errinj_label =
