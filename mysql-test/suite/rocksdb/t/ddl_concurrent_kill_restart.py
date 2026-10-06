"""
Concurrent DDL + DML load for a MyRocks kill-and-restart test.

Modes:

  setup user host port rows
      Creates the work tables w_0..w_3 (each with `rows` rows of known
      content and two secondary keys) and the DML table d_0, and flushes the
      memtables so the starting state is durable under any setting.

  load user host port seconds done_file
      Runs DDL and DML workers until the server goes away (it is killed by
      the test) or `seconds` pass, then appends "done" to done_file.
      Work tables only get DDL that keeps their rows unchanged:
        - COPY ALTER: FORCE, ADD/DROP COLUMN with a default, ADD/DROP INDEX
        - INSTANT ADD COLUMN
        - RENAME TABLE w_i TO w_i_tmp, w_i_tmp TO w_i (a swap in two steps,
          so the table may be found under either name after a crash)
      Scratch tables get CREATE / INSERT / DROP. The DML table gets
      INSERT / UPDATE / DELETE.
      INPLACE ADD/DROP INDEX, TRUNCATE and plain CREATE/DROP of work tables
      are not used: a crash inside them hits known bugs that make the server
      crash or refuse to start (PS-11666..PS-11670), which would stop the
      test instead of reporting.

  check user host port rows
      Runs after the restart. Prints one line per unexpected problem and
      nothing else, so the output is stable:
        - a work table missing under both names, or present under both,
        - a work table without exactly `rows` rows through its primary key
          and every secondary key, or with a different id/k sum,
        - CHECK TABLE not OK,
        - the DML table with different row counts through its keys.
      Problems that match known crash-window bugs are written to the log
      file given by the KNOWN_LOG environment variable instead:
        - a table known to the server but missing in the MyRocks dictionary
          or the other way round (PS-11666, PS-11667, PS-11681),
        - a work table missing from both dictionaries while a #sql2-...
          table holds its data (PS-11681),
        - leftover #sql tables (PS-11680).
      Finally it renames a work table found under its temporary name back.

  cleanup user host port
"""
import os
import random
import sys
import threading
import time

import MySQLdb

GONE = {2006, 2013, 2003, 2002, 1053, 1927}
EXPECTED = {
    1050, 1051, 1146, 1205, 1213, 1060, 1091, 1061, 1054, 1062,
    3572, 1317, 1180, 1030,
}
NWORK = 4

stop = threading.Event()
lock = threading.Lock()
unexpected = []


def connect(args, db="test"):
  con = MySQLdb.connect(user=args[0], host=args[1], port=int(args[2]), db=db,
                        connect_timeout=5)
  con.autocommit(True)
  cur = con.cursor()
  cur.execute("SET SESSION lock_wait_timeout = 3")
  cur.execute("SET SESSION rocksdb_lock_wait_timeout = 3")
  return con, cur


def run(cur, sql):
  """Returns True on success, False on an expected error; stops on a
  disconnect."""
  try:
    cur.execute(sql)
    return True
  except Exception as e:  # pylint: disable=broad-except
    code = e.args[0] if e.args and isinstance(e.args[0], int) else -1
    if code in GONE or stop.is_set():
      stop.set()
      return False
    if code not in EXPECTED:
      with lock:
        unexpected.append("%s: %r" % (sql[:60], e))
    return False


def work_ddl(name):
  ops = [
      "ALTER TABLE %s FORCE, ALGORITHM=COPY",
      "ALTER TABLE %s ADD COLUMN ex INT DEFAULT 7, ALGORITHM=COPY",
      "ALTER TABLE %s DROP COLUMN ex, ALGORITHM=COPY",
      "ALTER TABLE %s ADD COLUMN ix INT DEFAULT 3, ALGORITHM=INSTANT",
      "ALTER TABLE %s DROP COLUMN ix, ALGORITHM=COPY",
      "ALTER TABLE %s ADD INDEX kx (v), ALGORITHM=COPY",
      "ALTER TABLE %s DROP INDEX kx, ALGORITHM=COPY",
  ]
  return random.choice(ops) % name


def ddl_worker(args, i):
  try:
    _, cur = connect(args)
  except Exception:  # pylint: disable=broad-except
    return
  name = "w_%d" % i
  while not stop.is_set():
    if random.random() < 0.2:
      if run(cur, "RENAME TABLE %s TO %s_tmp" % (name, name)):
        run(cur, "RENAME TABLE %s_tmp TO %s" % (name, name))
    else:
      run(cur, work_ddl(name))


def scratch_worker(args, i):
  try:
    _, cur = connect(args)
  except Exception:  # pylint: disable=broad-except
    return
  n = 0
  while not stop.is_set():
    t = "s_%d_%d" % (i, n % 3)
    n += 1
    run(cur, "CREATE TABLE IF NOT EXISTS %s (id INT PRIMARY KEY, k INT, "
        "KEY sk (k)) ENGINE=ROCKSDB" % t)
    run(cur, "INSERT IGNORE INTO %s VALUES %s" %
        (t, ",".join("(%d,%d)" % (j, j % 13) for j in range(50))))
    run(cur, "DROP TABLE IF EXISTS %s" % t)


def dml_worker(args, i):
  try:
    _, cur = connect(args)
  except Exception:  # pylint: disable=broad-except
    return
  while not stop.is_set():
    k = random.randint(0, 2000)
    r = random.random()
    if r < 0.5:
      run(cur, "INSERT INTO d_0 VALUES (%d, %d, REPEAT('d', %d)) "
          "ON DUPLICATE KEY UPDATE k = k + 1" % (k, k % 17, 1 + k % 40))
    elif r < 0.8:
      run(cur, "UPDATE d_0 SET k = k + 1 WHERE id = %d" % k)
    else:
      run(cur, "DELETE FROM d_0 WHERE id = %d" % k)


def bg_worker(args):
  try:
    _, cur = connect(args)
  except Exception:  # pylint: disable=broad-except
    return
  while not stop.is_set():
    run(cur, random.choice([
        "SET GLOBAL rocksdb_force_flush_memtable_now = 1",
        "SET GLOBAL rocksdb_compact_cf = 'default'",
        "SET GLOBAL rocksdb_signal_drop_index_thread = 1",
    ]))
    time.sleep(0.2)


def expected_sums(rows):
  return (rows, sum(range(rows)), sum(i % 101 for i in range(rows)))


def setup(args, rows):
  _, cur = connect(args)
  for i in range(NWORK):
    t = "w_%d" % i
    cur.execute("DROP TABLE IF EXISTS %s, %s_tmp" % (t, t))
    cur.execute("CREATE TABLE %s (id INT PRIMARY KEY, k INT, "
                "v VARCHAR(40), KEY sk (k)) ENGINE=ROCKSDB" % t)
    for base in range(0, rows, 500):
      cur.execute("INSERT INTO %s VALUES %s" % (t, ",".join(
          "(%d,%d,'v%d')" % (j, j % 101, j)
          for j in range(base, min(rows, base + 500)))))
  cur.execute("DROP TABLE IF EXISTS d_0")
  cur.execute("CREATE TABLE d_0 (id INT PRIMARY KEY, k INT, v VARCHAR(40), "
              "KEY sk (k)) ENGINE=ROCKSDB")
  cur.execute("SET GLOBAL rocksdb_force_flush_memtable_now = 1")


def load(args, seconds, done_file):
  threads = [threading.Thread(target=ddl_worker, args=(args, i))
             for i in range(NWORK)]
  threads += [threading.Thread(target=scratch_worker, args=(args, i))
              for i in range(2)]
  threads += [threading.Thread(target=dml_worker, args=(args, i))
              for i in range(3)]
  threads.append(threading.Thread(target=bg_worker, args=(args,)))
  for t in threads:
    t.daemon = True
    t.start()
  end = time.time() + seconds
  while time.time() < end and not stop.is_set():
    time.sleep(0.2)
  stop.set()
  for t in threads:
    t.join(10)
  with open(done_file, "a") as f:
    f.write("done\n")
  for u in unexpected:
    sys.stderr.write("load: unexpected: %s\n" % u)


def known(msg):
  path = os.environ.get("KNOWN_LOG")
  if path:
    with open(path, "a") as f:
      f.write(msg + "\n")


def check(args, rows):
  _, cur = connect(args)
  problems = []
  cur.execute("SELECT table_name FROM information_schema.tables "
              "WHERE table_schema = 'test'")
  dd_tables = set(r[0] for r in cur.fetchall())
  cur.execute("SELECT DISTINCT table_name FROM information_schema.rocksdb_ddl "
              "WHERE table_schema = 'test'")
  rdb_tables = set(r[0] for r in cur.fetchall())
  for t in sorted(dd_tables - rdb_tables):
    if t.startswith("#sql"):
      continue
    known("table %s in the server dictionary but not in MyRocks" % t)
  for t in sorted(rdb_tables - dd_tables):
    if t.startswith("#sql"):
      known("leftover %s in the MyRocks dictionary" % t)
    else:
      known("table %s in MyRocks but not in the server dictionary" % t)
  exp = expected_sums(rows)
  for i in range(NWORK):
    t = "w_%d" % i
    names = [n for n in (t, t + "_tmp") if n in dd_tables and n in rdb_tables]
    if len(names) != 1:
      if (t in dd_tables) != (t in rdb_tables) or \
         (t + "_tmp" in dd_tables) != (t + "_tmp" in rdb_tables):
        continue  # dictionary mismatch, reported as known above
      if not names and any(x.startswith("#sql2") for x in rdb_tables):
        # A kill between the two renames of a COPY ALTER: the table is gone
        # from both dictionaries and its data is under #sql2-... (PS-11681).
        known("table %s unavailable, data left under #sql2 (PS-11681)" % t)
        continue
      problems.append("%s: found under %d names (dd=%s rdb=%s)" % (
          t, len(names), sorted(x for x in dd_tables if x.startswith(t)),
          sorted(x for x in rdb_tables if x.startswith(t))))
      continue
    name = names[0]
    cur.execute("SELECT index_name FROM information_schema.statistics "
                "WHERE table_schema = 'test' AND table_name = %s "
                "AND seq_in_index = 1", (name,))
    indexes = [r[0] for r in cur.fetchall()]
    for idx in indexes:
      cur.execute("SELECT COUNT(*), COALESCE(SUM(id), 0), COALESCE(SUM(k), 0) "
                  "FROM %s FORCE INDEX (`%s`)" % (name, idx))
      got = tuple(int(x) for x in cur.fetchone())
      if got != exp:
        problems.append("%s via %s: got %r expected %r" % (t, idx, got, exp))
    cur.execute("CHECK TABLE %s" % name)
    for r in cur.fetchall():
      if r[2] in ("status", "error") and r[3] != "OK":
        problems.append("%s: CHECK TABLE %s" % (t, r[3]))
    if name != t:
      cur.execute("RENAME TABLE %s TO %s" % (name, t))
  cur.execute("SELECT COUNT(*) FROM d_0 FORCE INDEX (PRIMARY)")
  pk = cur.fetchone()[0]
  cur.execute("SELECT COUNT(*) FROM d_0 FORCE INDEX (sk)")
  sk = cur.fetchone()[0]
  if pk != sk:
    problems.append("d_0: %d rows through PRIMARY, %d through sk" % (pk, sk))
  for p in problems:
    print(p)


def cleanup(args):
  _, cur = connect(args)
  cur.execute("SELECT table_name FROM information_schema.tables "
              "WHERE table_schema = 'test'")
  for (t,) in cur.fetchall():
    if t.startswith(("w_", "s_", "d_")):
      cur.execute("DROP TABLE IF EXISTS `%s`" % t)


def main():
  args = sys.argv[2:5]
  mode = sys.argv[1]
  if mode == "setup":
    setup(args, int(sys.argv[5]))
  elif mode == "load":
    load(args, float(sys.argv[5]), sys.argv[6])
  elif mode == "check":
    check(args, int(sys.argv[5]))
  elif mode == "cleanup":
    cleanup(args)


if __name__ == "__main__":
  main()
