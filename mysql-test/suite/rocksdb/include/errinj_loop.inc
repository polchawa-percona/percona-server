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
# The state is $errinj_state followed by the full rows of every MyRocks
# table of the schema, read through the primary key (every column, also the
# ones no index covers; myrocks_inv_fullrow.inc).
# After the first injected failure that did not abort the server, and then
# after every $errinj_restart_every-th one (default 20; 1 = after every one),
# the server is restarted (never with $errinj_no_restart_legs = 1) and the
# state must be the same as before the restart. After every restart (also after an abort) the
# MyRocks invariants must hold (myrocks_invariants.inc). No internal DDL
# table (#sql*, #truncate_tmp#*) may be left after the statement, nor after
# the restart, that was not there before the statement
# (myrocks_inv_orphans.inc); after an abort, a #sql-* entry that exists only
# in the server data dictionary is accepted (see $inv_orphans_abort_ok
# there) and counted in errinj_stats.log.
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
#   $errinj_source   include file that runs the statement; the default,
#                    errinj_stmt.inc, runs $errinj_stmt. Others serve statement
#                    sequences that cannot run in a procedure (XA) or that use
#                    other connections. It must arm the injection with
#                    errinj_arm.inc in the connection to test (only when
#                    $errinj_arm is 1), and on return be on connection default
#                    and set $errinj_err (first error, 2013 if the server
#                    aborted), $errinj_left (the counter left in the armed
#                    connection) and, optionally, $errinj_must_be_after = 1
#                    when an abort may only recover the final state.
#   $errinj_after_restart_inc  include run after a restart, before the state
#                    is read (default errinj_noop.inc)
#   $errinj_no_restart_legs  1 to skip the restart after the injected
#                    failures that did not abort the server
#   $errinj_restart_every  restart after the first and then after every N-th
#                    injected failure that did not abort the server. Default:
#                    the environment variable HUNT_ERRINJ_RESTART_N, else 20,
#                    so that the tests stay within the default MTR test case
#                    timeout. Depth runs use 1 (a restart and the full checks
#                    after every injected failure), e.g.
#                    HUNT_ERRINJ_RESTART_N=1 ./mtr --big-test
#                    --testcase-timeout=240 --suite-timeout=900 ...
#                    (mysqltest variable names are case-insensitive, so the
#                    environment variable must not be named like any
#                    $errinj_* variable: $ERRINJ_RESTART_EVERY would read
#                    $errinj_restart_every itself)
#   $errinj_crash_orphans_ok  ticket of a known bug that leaves internal DDL
#                    tables after an abort (PS-11680: COPY ALTER is not
#                    atomic): such leftovers after an abort are not
#                    violations; one line naming the ticket is printed at
#                    the end of the loop instead
#
# Output: one line per violation, then one summary line without counts, so
# that the result does not depend on how many RocksDB calls are made.
#

if (!$errinj_max) {
  --let $errinj_max = 200
}
if (!$errinj_restart_every) {
  --let $errinj_restart_every = $HUNT_ERRINJ_RESTART_N
}
if (!$errinj_restart_every) {
  --let $errinj_restart_every = 20
}
if (!$errinj_source) {
  --let $errinj_source = suite/rocksdb/include/errinj_stmt.inc
}
if (!$errinj_after_restart_inc) {
  --let $errinj_after_restart_inc = suite/rocksdb/include/errinj_noop.inc
}
if (!$errinj_label) {
  --let $errinj_label = $errinj_class/$errinj_status
}

--disable_query_log
--disable_result_log

# Reference run without injection.
eval $errinj_reset;
--source suite/rocksdb/include/errinj_read_state.inc
--let $errinj_before = $errinj_state_now
--let $errinj_arm = 0
--source $errinj_source
--source suite/rocksdb/include/errinj_read_state.inc
--let $errinj_after = $errinj_state_now
if ($errinj_before == $errinj_after) {
  --echo # $errinj_label: the statement does not change the state
}

--let $errinj_n = 1
--let $errinj_done = 0
--let $errinj_violations = 0
--let $errinj_fired = 0
--let $errinj_errors = 0
--let $errinj_crashes = 0
--let $errinj_restarts = 0
--let $errinj_leg_k = 0
--let $errinj_crash_orphans = 0
--let $errinj_orph_abort_n = 0
while (!$errinj_done) {
  eval $errinj_reset;
  # Internal DDL tables that exist before the statement (left by an earlier
  # iteration, already reported there).
  --let $errinj_orph_base =
  --source suite/rocksdb/include/errinj_orphans_new.inc
  --let $errinj_orph_start = $errinj_orph_now
  --let $errinj_orph_seen =
  if ($errinj_crash) {
    --exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
  }
  --let $errinj_must_be_after = 0
  --let $errinj_arm = 1
  --source $errinj_source
  --let $errinj_arm = 0

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
    --source $errinj_after_restart_inc
    --disable_query_log
    --disable_result_log
    --source suite/rocksdb/include/errinj_read_state.inc
    --let $errinj_ok = 0
    if ($errinj_state_now == $errinj_before) {
      if (!$errinj_must_be_after) {
        --let $errinj_ok = 1
      }
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
    --let $errinj_leg_crashed = 1
    --source suite/rocksdb/include/errinj_leg_invariants.inc
    --let $errinj_leg_crashed = 0
  }

  if (!$errinj_crashed) {
    if ($errinj_crash) {
      --remove_file $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
    }
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
      --source suite/rocksdb/include/errinj_read_state.inc
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
      --let $errinj_orph_base = $errinj_orph_start
      --source suite/rocksdb/include/errinj_orphans_new.inc
      if ($errinj_orph_new) {
        --echo VIOLATION $errinj_label n=$errinj_n: internal tables left after the statement: $errinj_orph_new
        --inc $errinj_violations
      }
      --let $errinj_orph_seen = $errinj_orph_now
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
    # An injected failure that did not abort the server: restart, and the
    # state must survive the restart.
    --let $errinj_restart_now = 0
    if (!$errinj_crashed) {
      if (!$errinj_no_restart_legs) {
        --let $errinj_restart_now = `SELECT $errinj_leg_k % $errinj_restart_every = 0`
        --inc $errinj_leg_k
      }
    }
    if ($errinj_restart_now) {
      if (!$errinj_no_restart_legs) {
        --let $errinj_state_pre = $errinj_state_now
        --source suite/rocksdb/include/errinj_restart.inc
        --inc $errinj_restarts
        --source suite/rocksdb/include/errinj_read_state.inc
        if ($errinj_state_now != $errinj_state_pre) {
          --echo VIOLATION $errinj_label n=$errinj_n: the state changed across a restart
          --echo   before the restart: $errinj_state_pre
          --echo   after the restart:  $errinj_state_now
          --inc $errinj_violations
        }
        --source suite/rocksdb/include/errinj_leg_invariants.inc
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
--exec echo "$errinj_label: injected=$errinj_fired errors=$errinj_errors crashes=$errinj_crashes restarts=$errinj_restarts restart_every=$errinj_restart_every hidden_dd_after_abort=$errinj_orph_abort_n violations=$errinj_violations" >> $MYSQLTEST_VARDIR/log/errinj_stats.log
--enable_result_log
--enable_query_log

if ($errinj_crash_orphans) {
  --echo # $errinj_label: internal tables left after an abort ($errinj_crash_orphans_ok)
}
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
