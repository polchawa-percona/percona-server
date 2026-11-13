"""
Concurrent bulk load and DDL driver for MyRocks.

Worker kinds, all running at the same time against one server:

  partial a thread bulk loading new groups into a table with a partial
          secondary index (materialized or not, per
          rocksdb_bulk_load_partial_index) and checking it against the
          primary key
  load    each thread repeatedly bulk loads (rocksdb_bulk_load=1) a fresh
          batch of rows into its own table, finishes the bulk load, and
          verifies its rows through the primary and the secondary key.
          The session settings given on the command line (bulk load size,
          unsorted input, secondary keys, unique key check, merge buffers,
          commit in the middle, ...) are applied to these sessions.
  ddl     threads running ADD/DROP INDEX, TRUNCATE, RENAME and COPY ALTER on
          their own tables, which share the column families of the loaded
          tables
  bg      a thread forcing memtable flushes, manual compactions and drop-index
          thread wake-ups

A bulk-loading session never runs DDL on a table another session loads
(the case of PS-11684 is tested separately).

Only unexpected errors are printed, so the output is stable. Expected errors
(metadata lock timeouts, deadlocks, a concurrent DDL statement winning a race)
are counted, not printed.

Usage:
  ddl_concurrent_bulk_load.py user host port seconds load_threads ddl_threads
      rows 'var=value,var=value,...'
  ddl_concurrent_bulk_load.py user host port cleanup
"""
import MySQLdb
import os
import random
import sys
import threading

EXPECTED = {
    1050,  # ER_TABLE_EXISTS_ERROR
    1051,  # ER_BAD_TABLE_ERROR
    1091,  # ER_CANT_DROP_FIELD_OR_KEY
    1061,  # ER_DUP_KEYNAME
    1146,  # ER_NO_SUCH_TABLE
    1205,  # ER_LOCK_WAIT_TIMEOUT
    1213,  # ER_LOCK_DEADLOCK
}

CFS = ["bl_cf1", "rev:bl_cf2"]

lock = threading.Lock()
errors = []
expected_count = [0]
stop = threading.Event()


def report(msg):
  with lock:
    errors.append(msg)


def connect(args):
  con = MySQLdb.connect(user=args[0], host=args[1], port=int(args[2]),
                        db="test")
  con.autocommit(True)
  cur = con.cursor()
  cur.execute("SET SESSION lock_wait_timeout = 10")
  cur.execute("SET SESSION rocksdb_lock_wait_timeout = 10")
  return con, cur


def run(cur, sql, who):
  try:
    cur.execute(sql)
    return True
  except (MySQLdb.OperationalError, MySQLdb.ProgrammingError,
          MySQLdb.IntegrityError, MySQLdb.InternalError) as e:
    code = e.args[0]
    msg = str(e.args[1]) if len(e.args) > 1 else ""
  except Exception as e:  # pylint: disable=broad-except
    report("%s: %s: %r" % (who, sql[:60], e))
    return False
  if os.environ.get("BL_DEBUG"):
    report("dbg %s: %s: %d %s" % (who, sql[:40], code, msg[:60]))
  if code in EXPECTED:
    with lock:
      expected_count[0] += 1
  else:
    report("%s: %s: error %d %s" % (who, sql[:60], code, msg[:80]))
  return False


def create(cur, name, cf, who):
  # The primary key lives in the default column family and the secondary
  # keys in a shared one, so keys of many tables share SST files.
  return run(cur,
             "CREATE TABLE %s (id INT, k INT, u INT, v VARCHAR(64), "
             "PRIMARY KEY (id), KEY sk (k) COMMENT 'cfname=%s', "
             "UNIQUE KEY uk (u) COMMENT 'cfname=%s') ENGINE=ROCKSDB" %
             (name, cf, cf), who)


def sums(n0, n):
  ids = range(n0, n0 + n)
  return (n, sum(ids), sum(i * 7 % 101 for i in ids))


def check(cur, name, n0, n, who):
  exp = sums(n0, n)
  for idx in ("PRIMARY", "sk", "uk"):
    cur.execute("SELECT COUNT(*), COALESCE(SUM(id),0), COALESCE(SUM(k),0) "
                "FROM %s FORCE INDEX(%s)" % (name, idx))
    got = tuple(int(x) for x in cur.fetchone())
    if got != exp:
      report("%s: %s via %s: got %r expected %r" % (who, name, idx, got, exp))


def worker_load(args, n, rows, settings):
  who = "load%d" % n
  con, cur = connect(args)
  for s in settings:
    run(cur, "SET SESSION rocksdb_%s" % s, who)
  cur.execute("SELECT @@rocksdb_bulk_load_allow_unsorted, "
              "@@rocksdb_bulk_load_allow_sk")
  row = cur.fetchone()
  unsorted = int(row[0]) == 1
  # With rocksdb_bulk_load_allow_sk the secondary keys are bulk loaded too,
  # and bulk-loaded keys must not overlap existing keys (ER_OVERLAPPING_KEYS),
  # so every batch then goes into an emptied table.
  fresh = int(row[1]) == 1
  name = "bl_%d" % n
  create(cur, name, CFS[n % len(CFS)], who)
  loaded = 0
  while not stop.is_set():
    if fresh and loaded:
      run(cur, "TRUNCATE TABLE %s" % name, who)
      loaded = 0
    ids = list(range(loaded, loaded + rows))
    random.shuffle(ids)  # out-of-order input when bulk_load_allow_unsorted
    if not unsorted:
      ids.sort()
    run(cur, "SET SESSION rocksdb_bulk_load = 1", who)
    ok = True
    for chunk in range(0, rows, 100):
      vals = ",".join("(%d,%d,%d,REPEAT('x',%d))" % (i, i * 7 % 101, i,
                                                      1 + i % 60)
                      for i in ids[chunk:chunk + 100])
      ok = run(cur, "INSERT INTO %s VALUES %s" % (name, vals), who) and ok
    ok = run(cur, "SET SESSION rocksdb_bulk_load = 0", who) and ok
    if ok:
      loaded += rows
    check(cur, name, 0, loaded, who)
  con.close()


def worker_partial(args, n, rows):
  # Bulk loads into a table with a partial secondary index. Every batch adds
  # new groups (first key part), some above and some below the
  # materialization threshold, so batches never overlap existing keys.
  who = "partial%d" % n
  con, cur = connect(args)
  name = "bp_%d" % n
  run(cur, "CREATE TABLE %s (g INT, id INT, k INT, PRIMARY KEY (g, id), "
      "KEY pk (g, k) COMMENT 'cfname=%s;partial_group_keyparts=1;"
      "partial_group_threshold=5', KEY fk (g, k) COMMENT 'cfname=%s') "
      "ENGINE=ROCKSDB" % (name, CFS[0], CFS[0]), who)
  batch = 0
  total = [0, 0, 0]
  while not stop.is_set():
    vals = []
    for j in range(10):
      g = batch * 10 + j
      for i in range(2 if j % 3 == 0 else rows // 10):
        vals.append((g, i, (g * 31 + i * 7) % 97))
    run(cur, "SET SESSION rocksdb_bulk_load = 1", who)
    ok = run(cur, "INSERT INTO %s VALUES %s" %
             (name, ",".join("(%d,%d,%d)" % v for v in vals)), who)
    ok = run(cur, "SET SESSION rocksdb_bulk_load = 0", who) and ok
    if ok:
      total[0] += len(vals)
      total[1] += sum(v[0] for v in vals)
      total[2] += sum(v[2] for v in vals)
    for idx in ("PRIMARY", "pk", "fk"):
      cur.execute("SELECT COUNT(*), COALESCE(SUM(g),0), COALESCE(SUM(k),0) "
                  "FROM %s FORCE INDEX(%s)" % (name, idx))
      got = [int(x) for x in cur.fetchone()]
      if got != total:
        report("%s: %s via %s: got %r expected %r" % (who, name, idx, got,
                                                      total))
    batch += 1
  con.close()


def worker_ddl(args, n, rows):
  who = "ddl%d" % n
  con, cur = connect(args)
  name = "dd_%d" % n
  cf = CFS[n % len(CFS)]
  create(cur, name, cf, who)
  vals = ",".join("(%d,%d,%d,'y')" % (i, i * 7 % 101, i) for i in range(rows))
  run(cur, "INSERT INTO %s VALUES %s" % (name, vals), who)
  step = 0
  while not stop.is_set():
    op = step % 5
    if op == 0:
      run(cur, "ALTER TABLE %s DROP INDEX sk" % name, who)
      run(cur, "ALTER TABLE %s ADD INDEX sk (k) COMMENT 'cfname=%s'" %
          (name, cf), who)
    elif op == 1:
      run(cur, "ALTER TABLE %s FORCE, ALGORITHM=COPY" % name, who)
    elif op == 2:
      run(cur, "RENAME TABLE %s TO %s_r" % (name, name), who)
      run(cur, "RENAME TABLE %s_r TO %s" % (name, name), who)
    elif op == 3:
      run(cur, "TRUNCATE TABLE %s" % name, who)
      run(cur, "INSERT INTO %s VALUES %s" % (name, vals), who)
    else:
      check(cur, name, 0, rows, who)
    step += 1
  con.close()


def worker_bg(args):
  who = "bg"
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    stmts = ["SET GLOBAL rocksdb_force_flush_memtable_now = 1",
             "SET GLOBAL rocksdb_compact_cf = 'default'",
             "SET GLOBAL rocksdb_compact_cf = '%s'" % CFS[i % len(CFS)],
             "SET GLOBAL rocksdb_signal_drop_index_thread = 1"]
    run(cur, stmts[i % len(stmts)], who)
    i += 1
    stop.wait(0.2)
  con.close()


def cleanup(args):
  con, cur = connect(args)
  cur.execute("SHOW TABLES LIKE 'bl\\_%'")
  names = [r[0] for r in cur.fetchall()]
  for pat in ("dd", "bp"):
    cur.execute("SHOW TABLES LIKE '%s\\_%%'" % pat)
    names += [r[0] for r in cur.fetchall()]
  for t in names:
    cur.execute("DROP TABLE IF EXISTS %s" % t)
  con.close()


def main():
  args = sys.argv[1:4]
  cleanup(args)
  if sys.argv[4] == "cleanup":
    return
  seconds = int(sys.argv[4])
  n_load = int(sys.argv[5])
  n_ddl = int(sys.argv[6])
  rows = int(sys.argv[7])
  settings = [s for s in sys.argv[8].split(",") if s] if len(sys.argv) > 8 \
      else []
  threads = [threading.Thread(target=worker_load, args=(args, i, rows,
                                                        settings))
             for i in range(n_load)]
  threads += [threading.Thread(target=worker_ddl, args=(args, i, rows))
              for i in range(n_ddl)]
  threads.append(threading.Thread(target=worker_partial,
                                  args=(args, 0, rows)))
  threads.append(threading.Thread(target=worker_bg, args=(args,)))
  for t in threads:
    t.start()
  stop.wait(seconds)
  stop.set()
  for t in threads:
    t.join()
  for e in errors[:20]:
    print(e)
  print("unexpected errors: %d" % len(errors))
  sys.stderr.write("expected errors: %d\n" % expected_count[0])


if __name__ == "__main__":
  main()
