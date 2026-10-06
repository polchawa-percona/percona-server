#!/bin/bash
#
# Regenerates the Percona Server 8.0 MyRocks datadirs used by the
# rocksdb.upgrade_from_80* tests:
#   data80_rocks_upgrade.zip        clean shutdown
#   data80_rocks_upgrade_crash.zip  killed with SIGKILL while several
#                                   operations were in flight
#   data80_rocks_upgrade_xa.zip     killed with SIGKILL with prepared XA
#                                   transactions
#
# Usage: gen_upgrade_datadirs.sh <8.0 build or install dir> <output dir>
#
# The 8.0 build must include MyRocks (ha_rocksdb.so). The zips were last
# generated with Percona Server 8.0.46-37 (RelWithDebInfo). Copy the
# resulting zips to mysql-test/std_data/.

set -euo pipefail

B80=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)

find_bin() {
  for p in "$B80/runtime_output_directory/$1" "$B80/bin/$1"; do
    [ -x "$p" ] && { echo "$p"; return; }
  done
  echo "cannot find $1 under $B80" >&2
  exit 1
}
MYSQLD=$(find_bin mysqld)
MYSQL=$(find_bin mysql)
MYSQLADMIN=$(find_bin mysqladmin)
PLUGIN_DIR=$B80/plugin_output_directory
[ -d "$PLUGIN_DIR" ] || PLUGIN_DIR=$B80/lib/plugin

WORK=${WORK:-$(mktemp -d)}
cleanup() {
  [ -n "${PID:-}" ] && kill -9 "$PID" 2>/dev/null || true
  [ -n "${KEEP_WORK:-}" ] || rm -rf "$WORK"
}
trap cleanup EXIT
PORT=${PORT:-29806}

# Small InnoDB files keep the zips small.
COMMON_OPTS=(
  --no-defaults
  --lc-messages-dir="$B80/share"
  --innodb-redo-log-capacity=16M
  --innodb-buffer-pool-size=32M
  --innodb-undo-tablespaces=2
  --log-bin=binlog --server-id=1 --binlog-format=ROW
  --transaction-isolation=READ-COMMITTED
  --default-time-zone=+00:00
)

ROCKS_OPTS=(
  --plugin-dir="$PLUGIN_DIR"
  --plugin-load-add=ha_rocksdb.so
  --rocksdb
)

PID=
init_datadir() {
  local d=$1
  "$MYSQLD" "${COMMON_OPTS[@]}" --initialize-insecure --datadir="$d" \
    --log-error="$WORK/$(basename "$d").init.err"
}

start_server() {
  local d=$1
  shift
  "$MYSQLD" "${COMMON_OPTS[@]}" "${ROCKS_OPTS[@]}" "$@" --datadir="$d" \
    --socket="$WORK/mysqld.sock" --port="$PORT" --mysqlx=OFF \
    --log-error="$WORK/$(basename "$d").err" &
  PID=$!
  for _ in $(seq 1 120); do
    "$MYSQLADMIN" -uroot -S "$WORK/mysqld.sock" ping >/dev/null 2>&1 && return
    sleep 1
  done
  echo "server did not start, see $WORK/$(basename "$d").err" >&2
  cat "$WORK/$(basename "$d").err" >&2
  exit 1
}

sql() { "$MYSQL" -uroot -S "$WORK/mysqld.sock" --default-character-set=utf8mb4 "$@"; }

# MTR connects to the "test" schema when it restarts on these datadirs.
create_test_schema() { sql -e "CREATE DATABASE test"; }

stop_clean() {
  "$MYSQLADMIN" -uroot -S "$WORK/mysqld.sock" shutdown
  wait "$PID" || true
  PID=
}

stop_kill() {
  kill -9 "$PID"
  wait "$PID" 2>/dev/null || true
  PID=
}

zip_datadir() {
  local d=$1
  rm -f "$d"/auto.cnf "$d"/*.pem "$d"/*.err "$d"/*.pid
  (cd "$(dirname "$d")" && zip -qr9 "$OUT/$(basename "$d").zip" "$(basename "$d")")
  ls -l "$OUT/$(basename "$d").zip"
}

# Clean shutdown datadir.
D=$WORK/data80_rocks_upgrade
init_datadir "$D"
start_server "$D"
create_test_schema
sql < "$HERE/data80_rocks_upgrade.sql"
sql < "$HERE/upgrade_80_checks.sql"
sql -e "SET time_zone='+00:00';
        CALL upgcheck.collect('upg', 'expected',
             't_ttl_short,t_ttl_col,t_ttl_part');
        SELECT COUNT(*) AS expected_rows FROM upgcheck.expected;"
stop_clean
zip_datadir "$D"

# Crash datadir.
D=$WORK/data80_rocks_upgrade_crash
init_datadir "$D"
start_server "$D"
create_test_schema
sql < "$HERE/data80_rocks_upgrade_crash.sql"
sql < "$HERE/upgrade_80_checks.sql"
sql -e "CALL upgcheck.collect('upgc', 'expected',
             't_bulk,t_alter,t_drop');
        SELECT COUNT(*) AS expected_rows FROM upgcheck.expected;"
# A bulk load that is never finished: its rows stay in a temporary SST file.
(echo "SET SESSION rocksdb_bulk_load = 1;
       INSERT INTO upgc.t_bulk SELECT seq + 100, seq FROM upgc.seq1000
         WHERE seq < 500;
       SELECT SLEEP(3600);") | sql >/dev/null 2>&1 &
# t_drop is dropped after a flush; its data is reclaimed by the drop-index
# thread, which usually finishes before the kill.
sql -e "SET GLOBAL rocksdb_force_flush_memtable_now = 1;
        DROP TABLE upgc.t_drop;"
# A COPY ALTER killed while it fills its #sql table.
(echo "ALTER TABLE upgc.t_alter
         ADD COLUMN h CHAR(64) AS (SHA2(REPEAT(id, 4000000), 256)) STORED,
         ALGORITHM=COPY;") | sql >/dev/null 2>&1 &
for _ in $(seq 1 60); do
  sql -N -e "SELECT COUNT(*) FROM information_schema.processlist
             WHERE state = 'copy to tmp table'" | grep -q 1 && break
  sleep 0.5
done
sleep 3
ls "$D"/.rocksdb | grep -c bulk_load.tmp.sst || echo "no bulk load tmp file"
stop_kill
zip_datadir "$D"

# Prepared XA datadir.
D=$WORK/data80_rocks_upgrade_xa
init_datadir "$D"
start_server "$D"
create_test_schema
# Each XA transaction runs in its own session: starting a second XA
# transaction in a session after XA PREPARE crashes 8.0 MyRocks.
awk -v dir="$WORK" 'BEGIN { n = 0 } /^-- SESSION/ { n++ }
  { print > sprintf("%s/xa_%02d.sql", dir, n) }' \
  "$HERE/data80_rocks_upgrade_xa.sql"
for f in $(ls "$WORK"/xa_*.sql | LC_ALL=C sort); do sql < "$f"; done
sql -e "XA RECOVER"
stop_kill
zip_datadir "$D"
