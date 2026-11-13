#
# crashpoint_loop.inc
#
# Crash-point enumeration over the durable writes made by one statement.
#
# For N = 1, 2, 3, ... the statement is run again from the same starting
# state with rocksdb_debug_crash_on_nth_durable_write = N, so that the
# server aborts (after logging which point fired) before the N-th durable
# write made by the session is executed. The loop ends when the statement
# completes with the counter left above zero, i.e. when it made fewer
# than N durable writes.
#
# After every crash the server is restarted with the DEFAULT options (the
# default rocksdb_validate_tables, no repair mode: what a user gets) and
# it is verified that:
#   - the server and MyRocks started,
#   - exactly one new "crash-point hook fired" line is in the error log
#     (so the crash was made by the hook), whose site is printed,
#   - CHECK TABLE reports OK for every table in $crashpoint_tables,
#   - the recovered state (crashpoint_state()) is either the starting
#     state ($crashpoint_before) or the post-statement state
#     ($crashpoint_after), never a mix,
#   - no #sql% table is left in the MyRocks dictionary (except in the
#     windows of $crashpoint_leftover_ok, which print the ticket),
#   - the table is usable: the post-recovery probe ($crashpoint_probe)
#     inserts and deletes a row through every index, and the state is
#     unchanged afterwards.
# Windows listed in $crashpoint_skip are crashed and restarted like the
# others but not checked; each prints "skipped (<ticket>)". The number of
# violations must always be 0: a known bug is a skipped window that cites
# its ticket (and a disabled test that asserts the correct result), never
# an expected violation.
# If MyRocks refused to start with the default options, the violation is
# recorded and the server is restarted with rocksdb_validate_tables=2
# (and then with the default options if even that fails) so that the loop
# can go on.
# If a prepared XA transaction was recovered and $crashpoint_finish is
# set, it is finished that way, before the tables are checked and the
# state is compared.
# Before every window the schema cp is dropped and recreated by
# $crashpoint_reset, so leftovers of one window (also the hidden ones that
# exist only in the server data dictionary) cannot reach the next one.
#
# Parameters (set by the test before sourcing this file):
#   $crashpoint_reset    statement(s) that recreate the schema cp in the
#                        starting state (it must DROP and CREATE the
#                        schema)
#   $crashpoint_stmt     the statement under test (eval'd), or
#   $crashpoint_stmt_file an include file that runs it (sourced instead
#                        of $crashpoint_stmt when set)
#   $crashpoint_pkcol    primary key column passed to crashpoint_state()
#   $crashpoint_tables   comma-separated tables for CHECK TABLE (cp.t)
#   $crashpoint_probe    statement that runs the post-recovery probe,
#                        e.g. CALL crashpoint_probe(); the probe sets
#                        @crashpoint_probe_res to 'ok' or to an error
#                        message and must leave the data as it found it
#   $crashpoint_max      upper bound for N (default 50)
#   $crashpoint_label    text used in the summary line
#   $crashpoint_finish   (optional) statement(s) that finish a recovered
#                        prepared XA transaction, e.g. XA COMMIT 'c1'
#   $crashpoint_body     (optional) include file that runs the statement;
#                        defaults to crashpoint_body_src.inc when
#                        $crashpoint_stmt_file is set and to
#                        crashpoint_body_eval.inc otherwise
#   $crashpoint_skip     (optional) comma list of windows to run but not
#                        check, with $crashpoint_skip_ticket (the ticket
#                        printed for each)
#   $crashpoint_detail_window / $crashpoint_detail_like (optional) pin a
#                        window by meaning: the hook's log line of window
#                        $crashpoint_detail_window must name a detail
#                        (see RDB_CRASH_DETAIL) that, written as
#                        "<site> [<detail>]", matches the SQL LIKE
#                        pattern $crashpoint_detail_like, e.g. the rename
#                        of the rebuilt table to its final name. If the
#                        window crashes elsewhere, or is never reached,
#                        it is a violation: the sequence of durable
#                        writes changed and the window must be re-checked
#                        (and the number updated). Use it for every
#                        window that is skipped or run alone, so that the
#                        number cannot silently move to another write.
#   $crashpoint_leftover_ok (optional) comma list of windows where a #sql
#                        leftover in the MyRocks dictionary is known,
#                        with $crashpoint_leftover_ticket
#   $crashpoint_final_from (optional) first window after which the
#                        statement's effect is acknowledged to the
#                        client (XA PREPARE returned): from there the
#                        state after finishing must be the post-statement
#                        state
#   $crashpoint_prepared_windows (optional) comma list of windows in which
#                        a prepared XA transaction must still be there
#                        after the restart (acknowledged PREPARE, finish
#                        not yet written)
#   $crashpoint_from     (optional) first window to run (default 1); with
#                        $crashpoint_max it selects a single window
#   $crashpoint_neutral  (optional) when set, a correct recovery prints
#                        "recovered to a consistent state" instead of
#                        naming the starting or the post-statement state
#                        (for a window whose correct outcome may be
#                        either one)
# The starting and post-statement state fingerprints are taken from
# uninjected runs of the reset and the statement.
#
# Output: one line per crash point (# n=N: crashed at SITE, recovered to
# ...) and one summary line without raw counters; one VIOLATION line per
# failed check.
#

if (!$crashpoint_max) {
  --let $crashpoint_max = 50
}
if (!$crashpoint_label) {
  --let $crashpoint_label = workload
}
if (!$crashpoint_skip) {
  --let $crashpoint_skip = 0
}
if (!$crashpoint_leftover_ok) {
  --let $crashpoint_leftover_ok = 0
}
if (!$crashpoint_final_from) {
  --let $crashpoint_final_from = 0
}
if (!$crashpoint_prepared_windows) {
  --let $crashpoint_prepared_windows = 0
}
if (!$crashpoint_probe) {
  --die crashpoint_loop.inc: crashpoint_probe is required
}
--let $_cp_expect = $MYSQLTEST_VARDIR/tmp/mysqld.1.expect

# The statement is run through a body include: the loop body below must
# not contain a conditional --source with a possibly empty name, because
# mysqltest executes a --source that sits in an ignored block as soon as a
# while loop re-iterates above it.
if (!$crashpoint_body) {
  if ($crashpoint_stmt_file) {
    --let $crashpoint_body = suite/rocksdb/include/crashpoint_body_src.inc
  }
  if (!$crashpoint_stmt_file) {
    --let $crashpoint_body = suite/rocksdb/include/crashpoint_body_eval.inc
  }
}

# Reference run without the hook: it defines the post-statement state.
# abort_on_error stays off: the statement include may run commands that
# are errors in the reference run but harmless there; a real failure of
# the statement is caught by $cp_ref_err below.
--disable_query_log
--disable_abort_on_error
SET SESSION rocksdb_debug_crash_on_nth_durable_write = 0;
# Counts the hook lines of earlier loops of the same test.
--source suite/rocksdb/include/crashpoint_site.inc
--let $cp_fired_prev = $cp_fired
eval $crashpoint_reset;
--eval CALL crashpoint_state('$crashpoint_pkcol');
--let $crashpoint_before = `SELECT @crashpoint_state`
--source $crashpoint_body
--let $cp_ref_err = $mysql_errno
--eval CALL crashpoint_state('$crashpoint_pkcol');
--let $crashpoint_after = `SELECT @crashpoint_state`
# The probe must work on the post-statement state and leave it unchanged.
eval $crashpoint_probe;
--let $cp_probe_res = `SELECT @crashpoint_probe_res`
--eval CALL crashpoint_state('$crashpoint_pkcol');
--let $cp_state_now = `SELECT @crashpoint_state`
--enable_abort_on_error
--enable_query_log
if ($cp_ref_err) {
  --die the reference run of $crashpoint_label failed with error $cp_ref_err
}
if ($cp_probe_res != ok) {
  --die the probe failed in the reference run of $crashpoint_label: $cp_probe_res
}
if ($cp_state_now != $crashpoint_after) {
  --die the probe changed the post-statement state in the reference run of $crashpoint_label
}

if (!$crashpoint_from) {
  --let $crashpoint_from = 1
}
--let $crashpoint_n = $crashpoint_from
--let $cp_detail_seen = 0
--let $crashpoint_done = 0
--let $crashpoint_violations = 0
--let $crashpoint_crashes = 0
while (!$crashpoint_done) {
  eval $crashpoint_reset;
  --disable_query_log
  eval SET SESSION rocksdb_debug_crash_on_nth_durable_write = $crashpoint_n;
  --exec echo "wait" > $_cp_expect
  --enable_query_log

  --disable_abort_on_error
  --source $crashpoint_body
  --let $cp_body_err = $mysql_errno
  --enable_abort_on_error

  # Probe whether the server survived the body. A connection error (or a
  # client out-of-sync left by an abort in the middle of a multi-statement
  # batch) means the body crashed the server; a clean result means it did
  # not. The probe is the source of truth, because the body's own last
  # error is unreliable when several statements are issued in a row.
  --disable_query_log
  --disable_result_log
  --disable_abort_on_error
  --error 0,1040,1053,1317,2002,2003,2006,2013,2014,2016,2017,2055,ER_CLIENT_INTERACTION_TIMEOUT
  SELECT 1;
  --let $cp_probe = $mysql_errno
  --enable_abort_on_error
  --enable_result_log
  --enable_query_log

  --let $cp_crashed = 0
  if ($cp_probe != 0) {
    --let $cp_crashed = 1
  }

  if ($cp_crashed) {
    --inc $crashpoint_crashes
    --source include/wait_until_disconnected.inc
    # Restart with the default options.
    --let $_expect_file_name = $_cp_expect
    --source include/start_mysqld.inc
    --let $_expect_file_name =
    # The include files above turn the query and result logs back on.
    --disable_query_log
    # If MyRocks refused to start, record it and restart in the repair
    # mode, then with the defaults, so that the loop can go on.
    --let $cp_engine = `SELECT COUNT(*) FROM information_schema.engines WHERE engine = 'ROCKSDB' AND support IN ('YES', 'DEFAULT')`
    if (!$cp_engine) {
      --echo VIOLATION $crashpoint_label n=$crashpoint_n: MyRocks did not start after the abort with the default options
      --inc $crashpoint_violations
      --let $_expect_file_name = $_cp_expect
      --let $restart_parameters = restart: --loose-rocksdb_validate_tables=2
      --let $do_not_echo_parameters = 1
      --source include/start_mysqld.inc
      --let $restart_parameters =
      --let $do_not_echo_parameters =
      --let $_expect_file_name =
      --disable_query_log
    }
    # Which site fired, and was it the hook that crashed the server?
    --source suite/rocksdb/include/crashpoint_site.inc
    --let $cp_want = `SELECT $cp_fired_prev + 1`
    if ($cp_fired != $cp_want) {
      --echo VIOLATION $crashpoint_label n=$crashpoint_n: the server crashed but the error log has no new crash-point hook line
      --inc $crashpoint_violations
    }
    --let $cp_fired_prev = $cp_fired
    if ($crashpoint_detail_window) {
      if ($crashpoint_n == $crashpoint_detail_window) {
        --let $cp_detail_ok = `SELECT '$cp_site_full' LIKE '$crashpoint_detail_like'`
        if (!$cp_detail_ok) {
          --echo VIOLATION $crashpoint_label n=$crashpoint_n: the window is not the expected write: the hook fired at $cp_site_full, expected a detail like $crashpoint_detail_like (the sequence of durable writes changed: re-check the window)
          --inc $crashpoint_violations
        }
        --let $cp_detail_seen = 1
      }
    }
    --let $cp_skipped = `SELECT FIND_IN_SET($crashpoint_n, '$crashpoint_skip') > 0`
    if ($cp_skipped) {
      --echo # n=$crashpoint_n: crashed at $cp_site, skipped ($crashpoint_skip_ticket)
    }
    if (!$cp_skipped) {
      --let $cp_prep_win = `SELECT FIND_IN_SET($crashpoint_n, '$crashpoint_prepared_windows') > 0`
      # Finish a recovered prepared XA transaction, if the workload has one.
      --let $cp_xa = query_get_value(XA RECOVER, data, 1)
      if ($cp_xa == 'No such row') {
        if ($cp_prep_win) {
          --echo VIOLATION $crashpoint_label n=$crashpoint_n: the acknowledged prepared XA transaction was lost
          --inc $crashpoint_violations
        }
      }
      if ($cp_xa != 'No such row') {
        if ($crashpoint_finish != '') {
          eval $crashpoint_finish;
        }
      }
      # Nothing may be left prepared after the workload is finished.
      --let $cp_xa2 = query_get_value(XA RECOVER, data, 1)
      if ($cp_xa2 != 'No such row') {
        --echo VIOLATION $crashpoint_label n=$crashpoint_n: prepared XA transaction $cp_xa2 is still there
        --inc $crashpoint_violations
      }
      # Every table must stay internally consistent.
      --let $cp_t = $crashpoint_tables
      while ($cp_t) {
        --let $cp_one = `SELECT SUBSTRING_INDEX('$cp_t', ',', 1)`
        --let $cp_t = `SELECT IF(LOCATE(',', '$cp_t') > 0, SUBSTRING('$cp_t', LOCATE(',', '$cp_t') + 1), '')`
        --let $cp_chk = query_get_value(CHECK TABLE $cp_one, Msg_text, 1)
        if ($cp_chk != OK) {
          --echo VIOLATION $crashpoint_label n=$crashpoint_n: CHECK TABLE $cp_one: $cp_chk
          --inc $crashpoint_violations
        }
      }
      # Leftover internal tables in the MyRocks dictionary.
      --let $cp_left_cnt = `SELECT COUNT(*) FROM information_schema.rocksdb_ddl WHERE table_schema = 'cp' AND table_name LIKE '#sql%'`
      --let $cp_left_ok = `SELECT FIND_IN_SET($crashpoint_n, '$crashpoint_leftover_ok') > 0`
      if ($cp_left_cnt) {
        if ($cp_left_ok) {
          --echo # n=$crashpoint_n: #sql leftover in the MyRocks dictionary ($crashpoint_leftover_ticket)
        }
        if (!$cp_left_ok) {
          --echo VIOLATION $crashpoint_label n=$crashpoint_n: #sql leftover in the MyRocks dictionary
          --inc $crashpoint_violations
        }
      }
      --eval CALL crashpoint_state('$crashpoint_pkcol');
      --let $cp_state_now = `SELECT @crashpoint_state`
      --let $cp_recovered = violation
      if ($cp_state_now == $crashpoint_before) {
        --let $cp_recovered = starting
      }
      if ($cp_state_now == $crashpoint_after) {
        --let $cp_recovered = final
      }
      if ($cp_recovered == violation) {
        --echo VIOLATION $crashpoint_label n=$crashpoint_n: recovered state is neither the starting nor the post-statement state
        --echo   before:    $crashpoint_before
        --echo   after:     $crashpoint_after
        --echo   recovered: $cp_state_now
        --inc $crashpoint_violations
      }
      if ($cp_recovered != final) {
        if ($crashpoint_n >= $crashpoint_final_from) {
          if ($crashpoint_final_from) {
            --echo VIOLATION $crashpoint_label n=$crashpoint_n: the statement was acknowledged before this crash point but the recovered state is not the post-statement state
            --inc $crashpoint_violations
          }
        }
      }
      # The table must be usable after the recovery. The probe runs after
      # the state was taken and must leave the state unchanged. A server
      # that crashes on the probe is a violation.
      --exec echo "wait" > $_cp_expect
      --disable_abort_on_error
      eval $crashpoint_probe;
      --let $cp_probe_err = $mysql_errno
      --let $cp_probe_res = `SELECT @crashpoint_probe_res`
      --disable_result_log
      --error 0,1040,1053,1317,2002,2003,2006,2013,2014,2016,2017,2055,ER_CLIENT_INTERACTION_TIMEOUT
      SELECT 1;
      --let $cp_alive = $mysql_errno
      --enable_result_log
      --enable_abort_on_error
      if ($cp_alive != 0) {
        --echo VIOLATION $crashpoint_label n=$crashpoint_n: the server crashed on the post-recovery probe
        --inc $crashpoint_violations
        --source include/wait_until_disconnected.inc
        --let $_expect_file_name = $_cp_expect
        --source include/start_mysqld.inc
        --let $_expect_file_name =
        --disable_query_log
      }
      if ($cp_alive == 0) {
        if ($cp_probe_res != ok) {
          --echo VIOLATION $crashpoint_label n=$crashpoint_n: probe after the recovery failed: $cp_probe_res
          --inc $crashpoint_violations
        }
        --eval CALL crashpoint_state('$crashpoint_pkcol');
        --let $cp_state_after_probe = `SELECT @crashpoint_state`
        if ($cp_state_after_probe != $cp_state_now) {
          --echo VIOLATION $crashpoint_label n=$crashpoint_n: the probe changed the recovered state
          --inc $crashpoint_violations
        }
      }
      # One line per crash point, without the raw counter.
      if ($cp_recovered == violation) {
        --echo # n=$crashpoint_n: crashed at $cp_site, recovered to an invalid state
      }
      if ($crashpoint_neutral) {
        if ($cp_recovered != violation) {
          --echo # n=$crashpoint_n: crashed at $cp_site, recovered to a consistent state
        }
      }
      if (!$crashpoint_neutral) {
        if ($cp_recovered == starting) {
          --echo # n=$crashpoint_n: crashed at $cp_site, recovered to the starting state
        }
        if ($cp_recovered == final) {
          --echo # n=$crashpoint_n: crashed at $cp_site, recovered to the post-statement state
        }
      }
    }
  }

  if (!$cp_crashed) {
    --remove_file $_cp_expect
    if ($cp_body_err) {
      --echo VIOLATION $crashpoint_label n=$crashpoint_n: error $cp_body_err without a crash
      --inc $crashpoint_violations
      --let $crashpoint_done = 1
    }
    if (!$cp_body_err) {
      --let $cp_left = `SELECT @@session.rocksdb_debug_crash_on_nth_durable_write`
      --disable_query_log
      SET SESSION rocksdb_debug_crash_on_nth_durable_write = 0;
      --enable_query_log
      if ($cp_left) {
        # Fewer than N durable writes: nothing fired, the loop is complete.
        --let $crashpoint_done = 1
        --eval CALL crashpoint_state('$crashpoint_pkcol');
        --let $cp_state_now = `SELECT @crashpoint_state`
        if ($cp_state_now != $crashpoint_after) {
          --echo VIOLATION $crashpoint_label n=$crashpoint_n: the statement succeeded but the state differs from the reference run
          --echo   expected: $crashpoint_after
          --echo   now:      $cp_state_now
          --inc $crashpoint_violations
        }
      }
      if (!$cp_left) {
        --echo VIOLATION $crashpoint_label n=$crashpoint_n: the counter reached zero without a crash
        --inc $crashpoint_violations
        --let $crashpoint_done = 1
      }
    }
  }

  --inc $crashpoint_n
  if ($crashpoint_n > $crashpoint_max) {
    if ($crashpoint_from == 1) {
      --echo # $crashpoint_label: stopped at the loop bound $crashpoint_max
    }
    --let $crashpoint_done = 1
  }
}

# The last window may have been a crash: the expect file is still there.
--exec rm -f $_cp_expect
--enable_result_log
if ($crashpoint_detail_window) {
  if (!$cp_detail_seen) {
    --echo VIOLATION $crashpoint_label n=$crashpoint_detail_window: the window was never crashed (the statement made fewer durable writes or the window is outside the range); re-check it
    --inc $crashpoint_violations
  }
}
if (!$crashpoint_crashes) {
  --echo # $crashpoint_label: no durable write was made by the statement
}
if ($crashpoint_crashes) {
  if (!$crashpoint_violations) {
    --echo # $crashpoint_label: $crashpoint_crashes crash points enumerated, no violations
  }
  if ($crashpoint_violations) {
    --echo # $crashpoint_label: $crashpoint_crashes crash points enumerated, $crashpoint_violations violations
  }
}
# The violation count is never recorded as expected output: a known bug
# is a skipped window with a ticket, so the loop always requires 0.
if ($crashpoint_violations) {
  --die $crashpoint_label: $crashpoint_violations crash-window violations; see the VIOLATION lines above
}
--enable_query_log

--let $crashpoint_n =
--let $crashpoint_done =
--let $crashpoint_body =
--let $crashpoint_stmt_file =
--let $crashpoint_violations =
--let $crashpoint_crashes =
--let $crashpoint_from =
--let $crashpoint_neutral =
--let $crashpoint_skip =
--let $crashpoint_skip_ticket =
--let $crashpoint_leftover_ok =
--let $crashpoint_leftover_ticket =
--let $crashpoint_detail_window =
--let $crashpoint_detail_like =
--let $cp_detail_seen =
--let $cp_site_full =
--let $crashpoint_final_from =
--let $crashpoint_prepared_windows =
--let $cp_ref_err =
--let $cp_body_err =
--let $cp_probe =
--let $cp_crashed =
--let $cp_left =
--let $cp_state_now =
--let $cp_recovered =
--let $cp_xa =
--let $cp_xa2 =
--let $cp_t =
--let $cp_one =
--let $cp_chk =
--let $cp_engine =
--let $cp_site =
--let $cp_fired =
--let $cp_fired_prev =
--let $wait_counter =
