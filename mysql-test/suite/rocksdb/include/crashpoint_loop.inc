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
#   - no internal DDL table (#sql*, #sql2*, #truncate_tmp#*) is left in
#     the MyRocks dictionary or, on debug builds, in the server data
#     dictionary (myrocks_inv_orphans.inc; except in the windows of
#     $crashpoint_leftover_ok, which print the ticket; a #sql-* entry that
#     exists only in the server data dictionary is accepted after a crash,
#     see $inv_orphans_abort_ok there, and counted in
#     $MYSQLTEST_VARDIR/log/crashpoint_stats.log),
#   - the MyRocks invariants hold for schema cp (myrocks_invariants.inc:
#     every secondary index matches the primary key, the MyRocks
#     dictionary matches the server data dictionary, the drop queue
#     drains),
#   - the table is usable: the post-recovery probe ($crashpoint_probe)
#     inserts and deletes a row through every index, and the state is
#     unchanged afterwards.
# Power loss: when the server runs with rocksdb_debug_unsynced_data_loss=ON
# the crash also loses every unsynced write to RocksDB files and removes the
# files whose directory entries were never synced (see the hook), so the
# same checks run against what a power cut would leave. Every hook crash
# must then have a matching "unsynced data dropped" line in the error log
# (none without the option). The starting state is created with
# rocksdb_flush_log_at_trx_commit=1 and the configured value is restored
# before the statement, so that the lazy settings (2, 0) cannot lose the
# starting state itself.
# With $crashpoint_after_window the loop ends with one more crash, after the
# statement completed and was acknowledged to the client (a power cut right
# after it, fired through the checkpoint site by power_loss_now.inc): the
# recovered state must then be the post-statement state in every mode.
# Windows listed in $crashpoint_skip are crashed and restarted like the
# others but not checked; each prints "skipped (<ticket>)". The number of
# violations must always be 0: a known bug is a skipped window that cites
# its ticket (and a disabled test that asserts the correct result), never
# an expected violation.
# If MyRocks refused to start with the default options (the server runs,
# the plugin did not initialize), the violation is recorded and the server
# is shut down and started again in the repair mode
# ($crashpoint_repair_parameters, default rocksdb_validate_tables=2), so
# that the window is still checked and the loop can go on. If MyRocks is
# inactive also then, that is recorded as a second violation and the loop
# ends with this window ("the remaining windows ... are skipped"): the
# summary line and the final --die still run, and the server is left
# running without MyRocks for MTR to clean up after the failed test. The
# same holds when $crashpoint_reset fails before a window (e.g. ER 8010
# after a recovery in the repair mode left MyRocks entries of schema cp).
# Not handled: when the crash left a prepared MyRocks transaction, the
# start without MyRocks runs the binlog crash recovery without it, and the
# repair-mode start can then abort ("Found 1 prepared transactions",
# "Can't init tc log"); MTR then reports the server as failed during the
# test instead of this loop printing its summary.
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
#   $crashpoint_after_window (optional) when set, add the crash after the
#                        completed statement, see above
#   $crashpoint_from     (optional) first window to run (default 1); with
#                        $crashpoint_max it selects a single window
#   $crashpoint_repair_parameters (optional) restart parameters of the
#                        repair-mode start after MyRocks did not start,
#                        in restart_mysqld.inc form (default
#                        "restart: --loose-rocksdb_validate_tables=2")
#   $crashpoint_no_die   (optional, self-test only) do not --die at the
#                        end when there were violations; the count is
#                        returned in $crashpoint_result_violations
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
--let $_cp_repair = restart: --loose-rocksdb_validate_tables=2
if ($crashpoint_repair_parameters) {
  --let $_cp_repair = $crashpoint_repair_parameters
}
--let $_cp_power = `SELECT @@global.rocksdb_debug_unsynced_data_loss`
--let $_cp_mode = `SELECT @@global.rocksdb_flush_log_at_trx_commit`
--let $_cp_lazy = `SELECT @@global.rocksdb_flush_log_at_trx_commit <> 1`

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
# A unique marker line in the error log: crashpoint_site.inc counts the
# hook and drop lines after it only, so lines written before this loop
# (earlier loops of the same test, earlier tests of the same worker)
# cannot shift the counts.
--let $cp_log_marker = `SELECT CONCAT('crashpoint_loop marker ', REPLACE(UUID(), '-', ''))`
--exec echo "$cp_log_marker" >> $MYSQLTEST_VARDIR/log/mysqld.1.err
--source suite/rocksdb/include/crashpoint_site.inc
--let $cp_fired_prev = $cp_fired
--let $cp_fired_base = $cp_fired
--let $cp_dropped_base = $cp_dropped
if ($_cp_lazy) {
  SET GLOBAL rocksdb_flush_log_at_trx_commit = 1;
}
eval $crashpoint_reset;
if ($_cp_lazy) {
  eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $_cp_mode;
}
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
  --echo REFERENCE PROBE FAILED: $cp_probe_res
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
--let $cp_hidden_dd = 0
--let $cp_after_mode = 0
--let $cp_dead = 0
--let $cp_dead_window = 0
--let $cp_win = n=$crashpoint_n
while (!$crashpoint_done) {
  --let $cp_win = n=$crashpoint_n
  if ($cp_after_mode) {
    --let $cp_win = after the statement
  }
  # The reset and the arming are not logged (the log state would otherwise
  # depend on the include files that ran before and on the combination).
  --disable_query_log
  if ($_cp_lazy) {
    SET GLOBAL rocksdb_flush_log_at_trx_commit = 1;
  }
  # A reset that fails (e.g. the MyRocks dictionary still has entries of
  # schema cp after a recovery in the repair mode: ER 8010) ends the loop
  # with a violation instead of stopping the test without a summary.
  --disable_result_log
  --disable_abort_on_error
  eval $crashpoint_reset;
  --let $cp_reset_err = $mysql_errno
  --enable_abort_on_error
  --enable_result_log
  if ($cp_reset_err) {
    --echo VIOLATION $crashpoint_label $cp_win: the reset of schema cp failed with error $cp_reset_err; the remaining windows of $crashpoint_label are skipped
    --inc $crashpoint_violations
    --let $cp_dead = 1
    --let $crashpoint_done = 1
  }
  if ($_cp_lazy) {
    eval SET GLOBAL rocksdb_flush_log_at_trx_commit = $_cp_mode;
  }
  # The rest of the window runs only while schema cp can be used (not
  # re-indented).
  if (!$cp_dead) {
  if (!$cp_after_mode) {
    eval SET SESSION rocksdb_debug_crash_on_nth_durable_write = $crashpoint_n;
  }
  --exec echo "wait" > $_cp_expect
  --enable_query_log

  --disable_abort_on_error
  --source $crashpoint_body
  --let $cp_body_err = $mysql_errno
  if ($cp_after_mode) {
    # The statement is done and acknowledged: cut the power now (the
    # hook fires at the checkpoint site, before anything is written).
    --disable_query_log
    SET SESSION rocksdb_debug_crash_on_nth_durable_write = 1;
    --disable_result_log
    --error 0,1040,1053,1317,2002,2003,2006,2013,2014,2016,2017,2055,ER_CLIENT_INTERACTION_TIMEOUT
    eval SET GLOBAL rocksdb_create_checkpoint = '$MYSQLTEST_VARDIR/tmp/power_loss_never';
    --enable_result_log
    --enable_query_log
  }
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
    # mode, so that the window is still checked and the loop can go on.
    --let $cp_engine = `SELECT COUNT(*) FROM information_schema.engines WHERE engine = 'ROCKSDB' AND support IN ('YES', 'DEFAULT')`
    if (!$cp_engine) {
      --echo VIOLATION $crashpoint_label $cp_win: MyRocks did not start after the abort with the default options
      --inc $crashpoint_violations
      # The server runs without MyRocks: shut it down and start it again
      # in the repair mode.
      --let $_cp_saved_restart = $restart_parameters
      --let $restart_parameters = $_cp_repair
      --let $do_not_echo_parameters = 1
      --source include/restart_mysqld.inc
      --let $restart_parameters = $_cp_saved_restart
      --let $do_not_echo_parameters =
      --disable_query_log
      --let $cp_engine = `SELECT COUNT(*) FROM information_schema.engines WHERE engine = 'ROCKSDB' AND support IN ('YES', 'DEFAULT')`
      if (!$cp_engine) {
        # Nothing of schema cp can be checked or reset any more: record it
        # and end the loop here (the summary and the final --die still run).
        --echo VIOLATION $crashpoint_label $cp_win: MyRocks inactive after restart, also in the repair mode; the remaining windows of $crashpoint_label are skipped
        --inc $crashpoint_violations
        --let $cp_dead = 1
        --let $crashpoint_done = 1
      }
    }
  }
  if ($cp_dead) {
    # Skip the checks of this window; the loop ends below.
    --let $cp_crashed = 0
    --let $cp_body_err = 0
    --let $cp_dead_window = 1
  }
  if ($cp_crashed) {
    # Which site fired, and was it the hook that crashed the server?
    --source suite/rocksdb/include/crashpoint_site.inc
    --let $cp_want = `SELECT $cp_fired_prev + 1`
    if ($cp_fired != $cp_want) {
      --echo VIOLATION $crashpoint_label $cp_win: the server crashed but the error log has no new crash-point hook line
      --inc $crashpoint_violations
    }
    --let $cp_fired_prev = $cp_fired
    # The power loss simulation drops the unsynced data in the hook, once
    # per crash; without it nothing may be dropped.
    --let $cp_drop_want = `SELECT $_cp_power * ($cp_fired - $cp_fired_base)`
    --let $cp_drop_have = `SELECT $cp_dropped - $cp_dropped_base`
    if ($cp_drop_have != $cp_drop_want) {
      --echo VIOLATION $crashpoint_label $cp_win: the unsynced data drop lines in the error log do not match the power loss setting
      --echo ORACLE-DIAG power=$_cp_power fired=$cp_fired fired_base=$cp_fired_base dropped=$cp_dropped dropped_base=$cp_dropped_base want=$cp_drop_want have=$cp_drop_have
      --inc $crashpoint_violations
    }
    if ($crashpoint_detail_window) {
      if ($crashpoint_n == $crashpoint_detail_window) {
        --let $cp_detail_ok = `SELECT '$cp_site_full' LIKE '$crashpoint_detail_like'`
        if (!$cp_detail_ok) {
          --echo VIOLATION $crashpoint_label $cp_win: the window is not the expected write: the hook fired at $cp_site_full, expected a detail like $crashpoint_detail_like (the sequence of durable writes changed: re-check the window)
          --inc $crashpoint_violations
        }
        --let $cp_detail_seen = 1
      }
    }
    --let $cp_skipped = `SELECT FIND_IN_SET($crashpoint_n, '$crashpoint_skip') > 0`
    if ($cp_skipped) {
      --echo # $cp_win: crashed at $cp_site, skipped ($crashpoint_skip_ticket)
    }
    if (!$cp_skipped) {
      --let $cp_prep_win = `SELECT FIND_IN_SET($crashpoint_n, '$crashpoint_prepared_windows') > 0`
      # Finish a recovered prepared XA transaction, if the workload has one.
      --let $cp_xa = query_get_value(XA RECOVER, data, 1)
      if ($cp_xa == 'No such row') {
        if ($cp_prep_win) {
          --echo VIOLATION $crashpoint_label $cp_win: the acknowledged prepared XA transaction was lost
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
        --echo VIOLATION $crashpoint_label $cp_win: prepared XA transaction $cp_xa2 is still there
        --inc $crashpoint_violations
      }
      # Every table must stay internally consistent.
      --let $cp_t = $crashpoint_tables
      while ($cp_t) {
        --let $cp_one = `SELECT SUBSTRING_INDEX('$cp_t', ',', 1)`
        --let $cp_t = `SELECT IF(LOCATE(',', '$cp_t') > 0, SUBSTRING('$cp_t', LOCATE(',', '$cp_t') + 1), '')`
        --let $cp_chk = query_get_value(CHECK TABLE $cp_one, Msg_text, 1)
        if ($cp_chk != OK) {
          --echo VIOLATION $crashpoint_label $cp_win: CHECK TABLE $cp_one: $cp_chk
          --inc $crashpoint_violations
        }
      }
      # Leftover internal tables in the MyRocks dictionary or the server
      # data dictionary.
      --let $inv_schema = cp
      --source suite/rocksdb/include/myrocks_inv_orphans.inc
      --disable_query_log
      --let $cp_left_ok = `SELECT FIND_IN_SET($crashpoint_n, '$crashpoint_leftover_ok') > 0`
      if ($inv_orphans_abort_ok != none) {
        --inc $cp_hidden_dd
        --eval SET @cp_orph = '$inv_orphans', @cp_orph_ok = '$inv_orphans_abort_ok'
        --let $inv_orphans = `SELECT IFNULL(GROUP_CONCAT(j.n ORDER BY j.n), 'none') FROM JSON_TABLE(CONCAT('["', REPLACE(@cp_orph, ',', '","'), '"]'), '$[*]' COLUMNS (n VARCHAR(512) PATH '$')) j WHERE NOT FIND_IN_SET(j.n, @cp_orph_ok)`
      }
      if ($inv_orphans != none) {
        if ($cp_left_ok) {
          --echo # $cp_win: #sql leftover in the MyRocks dictionary ($crashpoint_leftover_ticket)
        }
        if (!$cp_left_ok) {
          --echo VIOLATION $crashpoint_label $cp_win: internal DDL table left: $inv_orphans
          --inc $crashpoint_violations
        }
      }
      # The MyRocks invariants of schema cp (orphans are checked above).
      --let $inv_skip_orphans = 1
      --source suite/rocksdb/include/myrocks_invariants.inc
      --disable_query_log
      --let $inv_skip_orphans =
      --let $inv_schema =
      if ($inv_violations) {
        --echo VIOLATION $crashpoint_label $cp_win: MyRocks invariants violated after the recovery (INVARIANT VIOLATION rows above)
        --inc $crashpoint_violations
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
        --echo VIOLATION $crashpoint_label $cp_win: recovered state is neither the starting nor the post-statement state
        --echo   before:    $crashpoint_before
        --echo   after:     $crashpoint_after
        --echo   recovered: $cp_state_now
        --inc $crashpoint_violations
      }
      if ($cp_after_mode) {
        if ($cp_recovered != final) {
          --echo VIOLATION $crashpoint_label $cp_win: the acknowledged statement was lost by the crash that followed it
          --inc $crashpoint_violations
        }
      }
      if ($cp_recovered != final) {
        if ($crashpoint_n >= $crashpoint_final_from) {
          if ($crashpoint_final_from) {
            --echo VIOLATION $crashpoint_label $cp_win: the statement was acknowledged before this crash point but the recovered state is not the post-statement state
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
        --echo VIOLATION $crashpoint_label $cp_win: the server crashed on the post-recovery probe
        --inc $crashpoint_violations
        --source include/wait_until_disconnected.inc
        --let $_expect_file_name = $_cp_expect
        --source include/start_mysqld.inc
        --let $_expect_file_name =
        --disable_query_log
      }
      if ($cp_alive == 0) {
        if ($cp_probe_res != ok) {
          --echo VIOLATION $crashpoint_label $cp_win: probe after the recovery failed: $cp_probe_res
          --inc $crashpoint_violations
        }
        --eval CALL crashpoint_state('$crashpoint_pkcol');
        --let $cp_state_after_probe = `SELECT @crashpoint_state`
        if ($cp_state_after_probe != $cp_state_now) {
          --echo VIOLATION $crashpoint_label $cp_win: the probe changed the recovered state
          --inc $crashpoint_violations
        }
      }
      # One line per crash point, without the raw counter.
      if ($cp_recovered == violation) {
        --echo # $cp_win: crashed at $cp_site, recovered to an invalid state
      }
      if ($crashpoint_neutral) {
        if ($cp_recovered != violation) {
          --echo # $cp_win: crashed at $cp_site, recovered to a consistent state
        }
      }
      if (!$crashpoint_neutral) {
        if ($cp_recovered == starting) {
          --echo # $cp_win: crashed at $cp_site, recovered to the starting state
        }
        if ($cp_recovered == final) {
          --echo # $cp_win: crashed at $cp_site, recovered to the post-statement state
        }
      }
    }
  }

  if ($cp_crashed) {
    if ($cp_after_mode) {
      --let $crashpoint_done = 1
    }
  }
  if ($cp_dead_window) {
    --let $cp_crashed = 1
  }
  if (!$cp_crashed) {
    --remove_file $_cp_expect
    if ($cp_body_err) {
      --echo VIOLATION $crashpoint_label $cp_win: error $cp_body_err without a crash
      --inc $crashpoint_violations
      --let $crashpoint_done = 1
    }
    if (!$cp_body_err) {
      --let $cp_left = `SELECT @@session.rocksdb_debug_crash_on_nth_durable_write`
      --disable_query_log
      SET SESSION rocksdb_debug_crash_on_nth_durable_write = 0;
      --enable_query_log
      if ($cp_left) {
        # Fewer than N durable writes: nothing fired, the loop is complete
        # (after the crash that follows the completed statement, if asked).
        --let $crashpoint_done = 1
        if ($crashpoint_after_window) {
          --let $crashpoint_done = 0
          --let $cp_after_mode = 1
        }
        --eval CALL crashpoint_state('$crashpoint_pkcol');
        --let $cp_state_now = `SELECT @crashpoint_state`
        if ($cp_state_now != $crashpoint_after) {
          --echo VIOLATION $crashpoint_label $cp_win: the statement succeeded but the state differs from the reference run
          --echo   expected: $crashpoint_after
          --echo   now:      $cp_state_now
          --inc $crashpoint_violations
        }
      }
      if (!$cp_left) {
        --echo VIOLATION $crashpoint_label $cp_win: the counter reached zero without a crash
        --inc $crashpoint_violations
        --let $crashpoint_done = 1
      }
    }
  }
  }

  --inc $crashpoint_n
  if ($cp_after_mode) {
    # The crash after the completed statement is not bounded by the loop.
    --let $crashpoint_n = -1
  }
  if ($crashpoint_n > $crashpoint_max) {
    if ($crashpoint_from == 1) {
      if (!$cp_dead) {
        --echo # $crashpoint_label: stopped at the loop bound $crashpoint_max
      }
    }
    --let $crashpoint_done = 1
  }
}

# The last window may have been a crash: the expect file is still there.
--exec rm -f $_cp_expect
# Debugging aid, not part of the result.
--exec echo "$crashpoint_label: crashes=$crashpoint_crashes hidden_dd_after_crash=$cp_hidden_dd violations=$crashpoint_violations" >> $MYSQLTEST_VARDIR/log/crashpoint_stats.log
--enable_result_log
if ($crashpoint_detail_window) {
  if (!$cp_detail_seen) {
    --echo VIOLATION $crashpoint_label n=$crashpoint_detail_window: the window was never crashed (the statement made fewer durable writes or the window is outside the range); re-check it
    --inc $crashpoint_violations
  }
}
if (!$crashpoint_crashes) {
  if (!$cp_dead) {
    --echo # $crashpoint_label: no durable write was made by the statement
  }
  if ($cp_dead) {
    --echo # $crashpoint_label: no crash point enumerated, $crashpoint_violations violations
  }
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
--let $crashpoint_result_violations = $crashpoint_violations
if (!$crashpoint_no_die) {
  if ($crashpoint_violations) {
    --die $crashpoint_label: $crashpoint_violations crash-window violations; see the VIOLATION lines above
  }
}
--enable_query_log

--let $crashpoint_n =
--let $crashpoint_done =
--let $crashpoint_body =
--let $crashpoint_stmt_file =
--let $crashpoint_violations =
--let $crashpoint_crashes =
--let $crashpoint_from =
--let $crashpoint_after_window =
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
--let $cp_dropped =
--let $cp_win =
--let $cp_dead =
--let $cp_dead_window =
--let $cp_reset_err =
--let $_cp_saved_restart =
--let $crashpoint_repair_parameters =
--let $crashpoint_no_die =
--let $cp_after_mode =
--let $cp_drop_want =
--let $cp_drop_have =
--let $cp_fired_base =
--let $cp_dropped_base =
--let $cp_fired_prev =
--let $cp_log_marker =
--let $wait_counter =
