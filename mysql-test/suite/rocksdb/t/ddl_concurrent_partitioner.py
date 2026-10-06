"""
Concurrent bulk load / index build / DDL driver for MyRocks with
rocksdb_bulk_load_use_sst_partitioner=ON.

With the SST partitioner enabled, a session that bulk loads (or builds a
secondary index INPLACE) registers its index ids in the column family's SST
partitioner, so that compactions into the bottom level cut SST files at those
index boundaries. This driver runs, against one server and in the same
column families:

  bulk      threads creating their own table, bulk loading sorted rows into
            it (rocksdb_bulk_load=1), checking the rows through the primary
            and the secondary key, and usually dropping the table
  index     a thread adding and dropping a secondary index INPLACE on the
            survivor tables, checking the new index against the primary key
  dml       a thread updating survivor rows, creating several versions of
            the same keys
  snap      a thread holding consistent snapshots for a short time, so that
            compactions must keep old versions of the updated keys
  ddl       a thread creating, filling and dropping plain tables
  bg        a thread forcing memtable flushes, manual compactions of the
            column families and wake-ups of the drop-index thread

Only unexpected errors are printed, so the output is stable. Expected errors
(lock timeouts, deadlocks, a racing DDL statement) are counted, not printed.
With rocksdb_bulk_load_fail_if_not_bottommost_level=ON a bulk load may still
fail to place its files in the bottom level; that failure is expected, but
the table must then contain none of the loaded rows.

Usage:
  ddl_concurrent_partitioner.py user host port seconds bulk_threads rows
  ddl_concurrent_partitioner.py user host port cleanup
"""
import MySQLdb
import random
import sys
import threading
import time

EXPECTED = {
    1050,  # ER_TABLE_EXISTS_ERROR
    1051,  # ER_BAD_TABLE_ERROR
    1091,  # ER_CANT_DROP_FIELD_OR_KEY (index already dropped)
    1061,  # ER_DUP_KEYNAME (index already added)
    1146,  # ER_NO_SUCH_TABLE
    1205,  # ER_LOCK_WAIT_TIMEOUT
    1213,  # ER_LOCK_DEADLOCK
}
# ER_UNKNOWN_ERROR (1105), raised when a bulk load cannot place its files in
# the bottom level with rocksdb_bulk_load_fail_if_not_bottommost_level=ON.
BULK_NOT_BOTTOMMOST = 1105

CFS = ["cp_cf1", "rev:cp_cf2"]
SURVIVORS = 2

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
  cur.execute("SET SESSION lock_wait_timeout = 5")
  cur.execute("SET SESSION rocksdb_lock_wait_timeout = 5")
  cur.execute("SET SESSION rocksdb_bulk_load_use_sst_partitioner = ON")
  return con, cur


def run(cur, sql, who, extra_ok=()):
  """Returns None on success, else the error code."""
  try:
    cur.execute(sql)
    return None
  except (MySQLdb.OperationalError, MySQLdb.ProgrammingError,
          MySQLdb.IntegrityError) as e:
    code = e.args[0]
  except Exception as e:  # pylint: disable=broad-except
    report("%s: %s: %r" % (who, sql[:60], e))
    return -1
  if code in EXPECTED or code in extra_ok:
    with lock:
      expected_count[0] += 1
  else:
    report("%s: %s: error %d" % (who, sql[:60], code))
  return code


def ddl(name, cf, sk=True):
  return ("CREATE TABLE %s (id INT, k INT, v VARCHAR(64), PRIMARY KEY (id)"
          "%s) ENGINE=ROCKSDB" %
          (name, ", KEY sk (k) COMMENT 'cfname=%s'" % cf if sk else ""))


def values(rows, base=0):
  return ",".join("(%d,%d,REPEAT('x',%d))" % (i, i * 7 % 101, 1 + i % 60)
                  for i in range(base, base + rows))


def sums(cur, name, idx):
  cur.execute("SELECT COUNT(*), COALESCE(SUM(id),0), COALESCE(SUM(k),0) "
              "FROM %s FORCE INDEX(%s)" % (name, idx))
  return tuple(int(x) for x in cur.fetchone())


def expected(rows):
  return (rows, sum(range(rows)), sum(i * 7 % 101 for i in range(rows)))


def worker_bulk(args, n, rows):
  who = "bulk%d" % n
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    name = "bl_%d_%d" % (n, i % 3)
    cf = CFS[i % len(CFS)]
    i += 1
    run(cur, "DROP TABLE IF EXISTS %s" % name, who)
    if run(cur, ddl(name, cf), who) is not None:
      continue
    run(cur, "SET SESSION rocksdb_bulk_load_allow_sk = 1", who)
    run(cur, "SET SESSION rocksdb_bulk_load = 1", who)
    # Sorted, in two statements, so the bulk load spans statements.
    half = rows // 2
    ok = run(cur, "INSERT INTO %s VALUES %s" % (name, values(half)), who)
    ok2 = run(cur, "INSERT INTO %s VALUES %s" %
              (name, values(rows - half, half)), who)
    end = run(cur, "SET SESSION rocksdb_bulk_load = 0", who,
              extra_ok=(BULK_NOT_BOTTOMMOST,))
    if ok is None and ok2 is None:
      for idx in ("PRIMARY", "sk"):
        got = sums(cur, name, idx)
        if end is None and got != expected(rows):
          report("%s: %s via %s: got %r expected %r" %
                 (who, name, idx, got, expected(rows)))
        if end is not None and got[0] != 0:
          report("%s: %s via %s: failed bulk load left %d rows" %
                 (who, name, idx, got[0]))
    if random.random() < 0.7:
      run(cur, "DROP TABLE %s" % name, who)
  run(cur, "SET SESSION rocksdb_bulk_load = 0", who)
  con.close()


def worker_index(args):
  who = "index"
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    name = "survivor_%d" % (i % SURVIVORS)
    cf = CFS[i % len(CFS)]
    i += 1
    if run(cur, "ALTER TABLE %s ADD INDEX sk2 (k, id) COMMENT 'cfname=%s', "
           "ALGORITHM=INPLACE" % (name, cf), who) is None:
      a = sums(cur, name, "PRIMARY")
      b = sums(cur, name, "sk2")
      if a != b:
        report("%s: %s sk2 %r != PRIMARY %r" % (who, name, b, a))
    run(cur, "ALTER TABLE %s DROP INDEX sk2, ALGORITHM=INPLACE" % name, who)
  con.close()


def worker_dml(args, rows):
  who = "dml"
  con, cur = connect(args)
  while not stop.is_set():
    name = "survivor_%d" % random.randrange(SURVIVORS)
    lo = random.randrange(rows)
    # v changes, id and k stay, so the expected sums never change.
    run(cur, "UPDATE %s SET v = CONCAT(LEFT(v, 50), 'u') "
        "WHERE id BETWEEN %d AND %d" % (name, lo, lo + 20), who)
  con.close()


def worker_snap(args):
  who = "snap"
  con, cur = connect(args)
  while not stop.is_set():
    run(cur, "START TRANSACTION WITH CONSISTENT SNAPSHOT", who)
    run(cur, "SELECT COUNT(*) FROM survivor_0", who)
    cur.fetchall()
    time.sleep(random.uniform(0.1, 0.8))
    run(cur, "COMMIT", who)
  con.close()


def worker_ddl(args, rows):
  who = "ddl"
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    name = "plain_%d" % (i % 3)
    cf = CFS[i % len(CFS)]
    i += 1
    run(cur, "DROP TABLE IF EXISTS %s" % name, who)
    if run(cur, ddl(name, cf), who) is None:
      run(cur, "INSERT INTO %s VALUES %s" % (name, values(rows)), who)
  con.close()


def worker_bg(args):
  who = "bg"
  con, cur = connect(args)
  stmts = ["SET GLOBAL rocksdb_force_flush_memtable_now = 1",
           "SET GLOBAL rocksdb_compact_cf = 'default'",
           "SET GLOBAL rocksdb_compact_cf = '%s'" % CFS[0],
           "SET GLOBAL rocksdb_compact_cf = '%s'" % CFS[1],
           "SET GLOBAL rocksdb_signal_drop_index_thread = 1"]
  while not stop.is_set():
    run(cur, random.choice(stmts), who)
    time.sleep(random.uniform(0.05, 0.3))
  con.close()


def setup(args, rows):
  con, cur = connect(args)
  for n in range(SURVIVORS):
    name = "survivor_%d" % n
    cur.execute("DROP TABLE IF EXISTS %s" % name)
    cur.execute(ddl(name, CFS[n % len(CFS)]))
    cur.execute("INSERT INTO %s VALUES %s" % (name, values(rows)))
  con.close()


def cleanup(args):
  con, cur = connect(args)
  cur.execute("SHOW TABLES")
  for (t,) in cur.fetchall():
    if t.startswith(("bl_", "plain_")):
      cur.execute("DROP TABLE IF EXISTS %s" % t)
  con.close()


def main():
  args = sys.argv[1:]
  if len(args) >= 4 and args[3] == "cleanup":
    cleanup(args)
    return
  seconds, bulk_threads, rows = int(args[3]), int(args[4]), int(args[5])
  setup(args, rows)
  threads = [threading.Thread(target=worker_bulk, args=(args, n, rows))
             for n in range(bulk_threads)]
  threads += [threading.Thread(target=worker_index, args=(args,)),
              threading.Thread(target=worker_dml, args=(args, rows)),
              threading.Thread(target=worker_snap, args=(args,)),
              threading.Thread(target=worker_ddl, args=(args, rows)),
              threading.Thread(target=worker_bg, args=(args,))]
  for t in threads:
    t.start()
  time.sleep(seconds)
  stop.set()
  for t in threads:
    t.join()
  # Survivor content must be unchanged through both indexes.
  con, cur = connect(args)
  for n in range(SURVIVORS):
    name = "survivor_%d" % n
    for idx in ("PRIMARY", "sk"):
      got = sums(cur, name, idx)
      if got != expected(rows):
        report("final: %s via %s: got %r expected %r" %
               (name, idx, got, expected(rows)))
  con.close()
  cleanup(args)
  for e in sorted(set(errors)):
    print(e)
  sys.stderr.write("expected errors: %d\n" % expected_count[0])


if __name__ == "__main__":
  main()
