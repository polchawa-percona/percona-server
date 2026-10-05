"""
Concurrent CREATE TABLE / DROP TABLE / DROP DATABASE driver for MyRocks.

Runs several kinds of worker threads at the same time against one server:

  same      threads looping CREATE TABLE IF NOT EXISTS / INSERT / DROP TABLE
            IF EXISTS on one shared table name per column family
  own       each thread creates its own table, fills it, verifies its rows
            through the primary and the secondary key, and drops it
  dropdb    a thread creating a database with several tables, filling them and
            dropping the whole database
  bg        a thread forcing memtable flushes, manual compactions of the used
            column families and wake-ups of the drop-index thread

Survivor tables with known content live in the same column families for the
whole run; their checksums are compared at the end.

Only unexpected errors are printed, so the output is stable. Expected errors
(a concurrent DDL statement winning the race: table exists, unknown table,
metadata lock timeout, deadlock) are counted, not printed.

Usage:
  ddl_concurrent_create_drop.py user host port seconds threads_same \
      threads_own rows [cfdrop]
  ddl_concurrent_create_drop.py user host port cleanup

  cfdrop    adds two threads creating and dropping tables in the column
            families cc_dyn_0/1 and one thread repeatedly dropping those
            column families (SET GLOBAL rocksdb_delete_cf)
"""
import MySQLdb
import random
import sys
import threading
import time

EXPECTED = {
    1050,  # ER_TABLE_EXISTS_ERROR
    1051,  # ER_BAD_TABLE_ERROR
    1146,  # ER_NO_SUCH_TABLE
    1205,  # ER_LOCK_WAIT_TIMEOUT
    1213,  # ER_LOCK_DEADLOCK
    1007,  # ER_DB_CREATE_EXISTS
    1008,  # ER_DB_DROP_EXISTS
    1049,  # ER_BAD_DB_ERROR
    1210,  # ER_WRONG_ARGUMENTS (DDL on a column family being dropped)
    8018,  # ER_CF_DROPPED
    8019,  # ER_CANT_DROP_CF
}

CFS = ["cc_cf1", "rev:cc_cf2"]

lock = threading.Lock()
errors = []
expected_count = [0]
stop = threading.Event()


def report(msg):
  with lock:
    errors.append(msg)


def connect(args, db="test"):
  con = MySQLdb.connect(user=args[0], host=args[1], port=int(args[2]), db=db)
  con.autocommit(True)
  cur = con.cursor()
  cur.execute("SET SESSION lock_wait_timeout = 5")
  cur.execute("SET SESSION rocksdb_lock_wait_timeout = 5")
  return con, cur


def run(cur, sql, who):
  try:
    cur.execute(sql)
    return True
  except MySQLdb.OperationalError as e:
    code = e.args[0]
  except MySQLdb.ProgrammingError as e:
    code = e.args[0]
  except MySQLdb.IntegrityError as e:
    code = e.args[0]
  except Exception as e:  # pylint: disable=broad-except
    report("%s: %s: %r" % (who, sql[:60], e))
    return False
  if code in EXPECTED:
    with lock:
      expected_count[0] += 1
  else:
    report("%s: %s: error %d" % (who, sql[:60], code))
  return False


def ddl(name, cf):
  # The primary key goes to the default column family and the secondary
  # key to the given one, so both kinds of column family are shared by many
  # tables.
  return ("CREATE TABLE %s (id INT, k INT, v VARCHAR(64), PRIMARY KEY (id), "
          "KEY sk (k) COMMENT 'cfname=%s') ENGINE=ROCKSDB" % (name, cf))


def fill(cur, name, rows, who, ignore=False):
  vals = ",".join("(%d,%d,REPEAT('x',%d))" % (i, i * 7 % 101, 1 + i % 60)
                  for i in range(rows))
  return run(cur, "INSERT %s INTO %s VALUES %s" %
             ("IGNORE" if ignore else "", name, vals), who)


def expected_sums(rows):
  return (rows, sum(range(rows)), sum(i * 7 % 101 for i in range(rows)))


def check(cur, name, rows, who):
  exp = expected_sums(rows)
  for idx in ("PRIMARY", "sk"):
    cur.execute("SELECT COUNT(*), COALESCE(SUM(id),0), COALESCE(SUM(k),0) "
                "FROM %s FORCE INDEX(%s)" % (name, idx))
    got = tuple(int(x) for x in cur.fetchone())
    if got != exp:
      report("%s: %s via %s: got %r expected %r" % (who, name, idx, got, exp))


def worker_same(args, n, rows):
  who = "same%d" % n
  con, cur = connect(args)
  while not stop.is_set():
    cf = CFS[random.randrange(len(CFS))]
    name = "shared_%s" % cf.replace(":", "_")
    run(cur, ddl(name, cf).replace("CREATE TABLE", "CREATE TABLE IF NOT EXISTS"),
        who)
    fill(cur, name, rows, who, ignore=True)
    run(cur, "DROP TABLE IF EXISTS %s" % name, who)
  con.close()


def worker_own(args, n, rows):
  who = "own%d" % n
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    name = "own_%d_%d" % (n, i % 3)
    cf = CFS[i % len(CFS)]
    i += 1
    run(cur, "DROP TABLE IF EXISTS %s" % name, who)
    if not run(cur, ddl(name, cf), who):
      continue
    if fill(cur, name, rows, who):
      check(cur, name, rows, who)
    if random.random() < 0.7:
      run(cur, "DROP TABLE %s" % name, who)
  con.close()


def worker_dropdb(args, rows):
  who = "dropdb"
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    db = "ccdb_%d" % (i % 2)
    i += 1
    run(cur, "DROP DATABASE IF EXISTS %s" % db, who)
    if not run(cur, "CREATE DATABASE %s" % db, who):
      continue
    for t in range(4):
      name = "%s.t%d" % (db, t)
      if run(cur, ddl(name, CFS[t % len(CFS)]), who):
        fill(cur, name, rows, who)
    run(cur, "DROP DATABASE %s" % db, who)
  con.close()


def worker_cf_user(args, n, rows):
  """Creates and drops tables whose secondary key uses a column family that
  another thread keeps trying to drop."""
  who = "cfuser%d" % n
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    name = "cfu_%d_%d" % (n, i % 2)
    cf = "cc_dyn_%d" % (i % 2)
    i += 1
    run(cur, "DROP TABLE IF EXISTS %s" % name, who)
    if not run(cur, ddl(name, cf), who):
      continue
    if fill(cur, name, rows, who):
      check(cur, name, rows, who)
    run(cur, "DROP TABLE %s" % name, who)
  con.close()


def worker_cf_dropper(args):
  who = "cfdrop"
  con, cur = connect(args)
  i = 0
  while not stop.is_set():
    run(cur, "SET GLOBAL rocksdb_delete_cf = 'cc_dyn_%d'" % (i % 2), who)
    i += 1
    time.sleep(0.01)
  con.close()


def worker_bg(args):
  who = "bg"
  con, cur = connect(args)
  while not stop.is_set():
    op = random.randrange(4)
    if op == 0:
      run(cur, "SET GLOBAL rocksdb_force_flush_memtable_now = 1", who)
    elif op == 1:
      run(cur, "SET GLOBAL rocksdb_compact_cf = '%s'" %
          random.choice(CFS + ["default"]), who)
    elif op == 2:
      run(cur, "SET GLOBAL rocksdb_signal_drop_index_thread = 1", who)
    else:
      run(cur, "SET GLOBAL rocksdb_force_flush_memtable_and_lzero_now = 1",
          who)
    time.sleep(0.05)
  con.close()


def cleanup(args):
  con, cur = connect(args)
  cur.execute("SELECT table_name FROM information_schema.tables WHERE "
              "table_schema = 'test' AND (table_name LIKE 'own\\_%' OR "
              "table_name LIKE 'shared\\_%' OR table_name LIKE 'cfu\\_%')")
  for (name,) in cur.fetchall():
    cur.execute("DROP TABLE IF EXISTS %s" % name)
  for i in range(2):
    cur.execute("DROP DATABASE IF EXISTS ccdb_%d" % i)
  con.close()
  return 0


def main():
  args = sys.argv[1:4]
  if sys.argv[4] == "cleanup":
    return cleanup(args)
  seconds = float(sys.argv[4])
  n_same = int(sys.argv[5])
  n_own = int(sys.argv[6])
  rows = int(sys.argv[7])

  con, cur = connect(args)
  survivors = ["survivor_%d" % i for i in range(len(CFS))]
  for i, s in enumerate(survivors):
    cur.execute("DROP TABLE IF EXISTS %s" % s)
    cur.execute(ddl(s, CFS[i]))
    fill(cur, s, rows * 4, "setup")

  threads = [threading.Thread(target=worker_same, args=(args, i, rows))
             for i in range(n_same)]
  threads += [threading.Thread(target=worker_own, args=(args, i, rows))
              for i in range(n_own)]
  threads.append(threading.Thread(target=worker_dropdb, args=(args, rows)))
  threads.append(threading.Thread(target=worker_bg, args=(args,)))
  if len(sys.argv) > 8 and sys.argv[8] == "cfdrop":
    threads += [threading.Thread(target=worker_cf_user, args=(args, i, rows))
                for i in range(2)]
    threads.append(threading.Thread(target=worker_cf_dropper, args=(args,)))
  for t in threads:
    t.start()
  time.sleep(seconds)
  stop.set()
  for t in threads:
    t.join()

  for s in survivors:
    check(cur, s, rows * 4, "final")
  con.close()

  for e in errors[:50]:
    print("UNEXPECTED: %s" % e)
  print("unexpected errors: %d" % len(errors))
  sys.stderr.write("expected (race) errors: %d\n" % expected_count[0])
  return 1 if errors else 0


if __name__ == "__main__":
  sys.exit(main())
