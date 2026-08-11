#!/bin/bash
# Bounded-timeout probe: opens a fresh connection and issues an operation
# (a global buffer-pool flush) that needs to acquire EVERY Fil_shard's mutex,
# including the one the race just exercised. If that mutex leaked (bug
# present), this call hangs until the `timeout` wrapper kills it -- in a
# non-debug/release build. In a debug build, per this investigation, the
# leak is instead caught immediately as a SIGABRT inside the SAME do_io()
# call that leaked it (dblwr::write()'s own DB_PAGE_IS_STALE handling calls
# was_stale() -> fil_space_t::was_not_deleted(), which asserts the shard
# mutex is NOT held -- but it still is), so by the time this probe would run
# the server is usually already dead. Either way this probe returns quickly
# when the fix is applied.
#
# Set MYSQL_BIN_DIR (dir containing the built `mysql` client) and
# MYSQLD_SOCKET (the running debug server's socket) before invoking this
# from repro.test's --exec line, e.g.:
#   MYSQL_BIN_DIR=/tmp/mybuild/runtime_output_directory \
#   MYSQLD_SOCKET=/tmp/myrepro/mysqld.sock \
#   ./probe.sh
MYSQL_BIN="${MYSQL_BIN_DIR:-/tmp/bug39244016-84-build/runtime_output_directory}/mysql"
SOCK="${MYSQLD_SOCKET:-/tmp/bug39244016-repro/data/mysqld.sock}"

timeout 10 "$MYSQL_BIN" --no-defaults -uroot -S "$SOCK" -e \
  "SET GLOBAL innodb_buf_flush_list_now = ON; SELECT 'PROBE_OK' AS result;"
rc=$?
if [ $rc -eq 124 ]; then
  echo "PROBE_RESULT: HUNG (timed out after 10s -- mutex leak, bug present)"
else
  echo "PROBE_RESULT: OK (exit $rc, no hang)"
fi
