"""Concurrent TRUNCATE TABLE / RENAME TABLE load for MyRocks.

Several connections run, at the same time and many times over:
  - TRUNCATE TABLE on random tables, followed by re-inserting rows;
  - RENAME TABLE swaps (a -> tmp, b -> a, tmp -> b) inside one schema;
  - RENAME TABLE moves to another schema and back;
  - ALTER TABLE ... TRUNCATE PARTITION on a partitioned table;
  - INSERT / UPDATE / DELETE on all tables;
  - readers comparing primary key and secondary key counts in one statement;
  - memtable flushes, manual compactions and drop-index thread wake-ups.

The tables live in two column families (default and cf_cc2), so with a small
write buffer every SST file holds keys of many tables and indexes at once,
including indexes being dropped by TRUNCATE.

Every table has an "owner" column whose DEFAULT is the table's identity.
Rows are inserted without an owner value, so a row always carries the
identity of the table definition it was written through. RENAME and TRUNCATE
keep the definition, so at the end every row of a table must have that
table's own default: a row with another owner means that data of one table
became visible through another table (index id confusion).

The script prints only deterministic lines; any failure is printed with an
"ERROR:" prefix and makes the exit status non-zero.
"""

import argparse
import random
import sys
import threading
import time

import MySQLdb

# Errors that concurrent DDL may legitimately cause for a single statement.
ALLOWED = {
    1146,  # no such table (moved to the other schema at this moment)
    1205,  # lock wait timeout
    1213,  # deadlock / snapshot conflict
    1412,  # table definition changed
    1062,  # duplicate key
    1050,  # table exists (rename target taken by a concurrent rename)
}

N_TABLES = 6
PART_OWNER = 100


def connect(args, db=None):
  conn = MySQLdb.connect(host="localhost", user="root", db=db or "d1",
                         unix_socket=args.socket)
  conn.autocommit(True)
  cur = conn.cursor()
  cur.execute("SET SESSION rocksdb_lock_wait_timeout = 5")
  cur.execute("SET SESSION lock_wait_timeout = 30")
  return conn


class Worker(threading.Thread):

  def __init__(self, args, name, iterations, seed):
    super().__init__(name=name)
    self.args = args
    self.iterations = iterations
    self.rnd = random.Random(seed)
    self.errors = []
    self.ops = 0
    self.skipped = {}

  def q(self, cur, sql):
    try:
      cur.execute(sql)
      self.ops += 1
      return cur.fetchall()
    except MySQLdb.OperationalError as e:
      if e.args[0] in ALLOWED:
        self.skipped[e.args[0]] = self.skipped.get(e.args[0], 0) + 1
        return None
      self.errors.append("%s: %s -> %s" % (self.name, sql, e))
      raise
    except MySQLdb.Error as e:
      if e.args and e.args[0] in ALLOWED:
        self.skipped[e.args[0]] = self.skipped.get(e.args[0], 0) + 1
        return None
      self.errors.append("%s: %s -> %s" % (self.name, sql, e))
      raise

  def run(self):
    try:
      conn = connect(self.args)
      cur = conn.cursor()
      for i in range(self.iterations):
        self.step(cur, i)
      conn.close()
    except Exception as e:  # pylint: disable=broad-except
      if not self.errors:
        self.errors.append("%s: %r" % (self.name, e))

  def table(self):
    return "t%d" % self.rnd.randrange(N_TABLES)


class Truncater(Worker):

  def step(self, cur, i):
    t = self.table()
    self.q(cur, "TRUNCATE TABLE d1.%s" % t)
    base = self.rnd.randrange(1000)
    values = ",".join("(%d,%d,'tr%d')" % (base + j, j, i) for j in range(20))
    self.q(cur, "INSERT IGNORE INTO d1.%s (id,k,pad) VALUES %s" % (t, values))


class Swapper(Worker):

  def step(self, cur, i):
    a, b = self.rnd.sample(range(N_TABLES), 2)
    tmp = "swap_%s" % self.name
    self.q(cur, "RENAME TABLE d1.t%d TO d1.%s, d1.t%d TO d1.t%d, d1.%s TO "
                "d1.t%d" % (a, tmp, b, a, tmp, b))


class Mover(Worker):

  def step(self, cur, i):
    t = self.table()
    if self.q(cur, "RENAME TABLE d1.%s TO d2.%s" % (t, t)) is not None:
      self.q(cur, "SELECT COUNT(*) FROM d2.%s" % t)
      # Must move back: a table left in d2 would break the next iterations.
      for _ in range(100):
        try:
          cur.execute("RENAME TABLE d2.%s TO d1.%s" % (t, t))
          break
        except MySQLdb.Error as e:
          if e.args[0] not in ALLOWED:
            self.errors.append("%s: move back %s -> %s" % (self.name, t, e))
            raise
          time.sleep(0.05)


class PartTruncater(Worker):

  def step(self, cur, i):
    p = self.rnd.randrange(4)
    self.q(cur, "ALTER TABLE d1.p TRUNCATE PARTITION p%d" % p)
    values = ",".join("(%d,%d,'pt%d')" % (p * 1000 + j, j, i) for j in range(20))
    self.q(cur, "INSERT IGNORE INTO d1.p (id,k,pad) VALUES %s" % values)


class Writer(Worker):

  def step(self, cur, i):
    t = self.table() if self.rnd.random() < 0.85 else "p"
    op = self.rnd.random()
    if op < 0.6:
      base = self.rnd.randrange(4000)
      values = ",".join("(%d,%d,'w%d')" % (base + j, self.rnd.randrange(50), i)
                        for j in range(25))
      self.q(cur, "INSERT INTO d1.%s (id,k,pad) VALUES %s ON DUPLICATE KEY "
                  "UPDATE k = VALUES(k), pad = VALUES(pad)" % (t, values))
    elif op < 0.8:
      lo = self.rnd.randrange(4000)
      self.q(cur, "UPDATE d1.%s SET k = k + 1 WHERE id BETWEEN %d AND %d" %
             (t, lo, lo + 30))
    else:
      lo = self.rnd.randrange(4000)
      self.q(cur, "DELETE FROM d1.%s WHERE id BETWEEN %d AND %d" %
             (t, lo, lo + 10))


class Reader(Worker):

  def step(self, cur, i):
    t = self.table() if self.rnd.random() < 0.85 else "p"
    r = self.q(cur, "SELECT (SELECT COUNT(*) FROM d1.%s FORCE INDEX(PRIMARY)),"
                    " (SELECT COUNT(*) FROM d1.%s FORCE INDEX(sk))" % (t, t))
    if r and r[0][0] != r[0][1]:
      self.errors.append("%s: d1.%s PK count %d != SK count %d" %
                         (self.name, t, r[0][0], r[0][1]))


class Maintenance(Worker):

  def step(self, cur, i):
    op = i % 4
    if op == 0:
      self.q(cur, "SET GLOBAL rocksdb_force_flush_memtable_now = 1")
    elif op == 1:
      self.q(cur, "SET GLOBAL rocksdb_compact_cf = 'cf_cc2'")
    elif op == 2:
      self.q(cur, "SET GLOBAL rocksdb_signal_drop_index_thread = 1")
    else:
      self.q(cur, "SET GLOBAL rocksdb_compact_cf = 'default'")
    time.sleep(0.05)


def setup(args):
  conn = connect(args, db="mysql")
  cur = conn.cursor()
  cur.execute("DROP DATABASE IF EXISTS d1")
  cur.execute("DROP DATABASE IF EXISTS d2")
  cur.execute("CREATE DATABASE d1")
  cur.execute("CREATE DATABASE d2")
  for i in range(N_TABLES):
    cf = "cfname=cf_cc2" if i % 2 else ""
    cur.execute(
        "CREATE TABLE d1.t%d (id INT NOT NULL, k INT, owner INT NOT NULL "
        "DEFAULT %d, pad VARCHAR(64), PRIMARY KEY (id) COMMENT '%s', "
        "KEY sk (k) COMMENT '%s') ENGINE=ROCKSDB" % (i, i, cf, cf))
    values = ",".join("(%d,%d,'init')" % (j, j % 50) for j in range(300))
    cur.execute("INSERT INTO d1.t%d (id,k,pad) VALUES %s" % (i, values))
  cur.execute(
      "CREATE TABLE d1.p (id INT NOT NULL, k INT, owner INT NOT NULL DEFAULT "
      "%d, pad VARCHAR(64), PRIMARY KEY (id) COMMENT 'cfname=cf_cc2', KEY sk "
      "(k)) ENGINE=ROCKSDB PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS "
      "THAN (1000), PARTITION p1 VALUES LESS THAN (2000), PARTITION p2 VALUES "
      "LESS THAN (3000), PARTITION p3 VALUES LESS THAN MAXVALUE)" % PART_OWNER)
  values = ",".join("(%d,%d,'init')" % (j * 7, j % 50) for j in range(600))
  cur.execute("INSERT INTO d1.p (id,k,pad) VALUES %s" % values)
  conn.close()


def check(args):
  """Checks run after all workers stopped. Returns a list of failures."""
  failures = []
  conn = connect(args, db="mysql")
  cur = conn.cursor()
  cur.execute("SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.TABLES "
              "WHERE TABLE_SCHEMA IN ('d1','d2') ORDER BY 1, 2")
  tables = cur.fetchall()
  names = sorted("%s.%s" % t for t in tables)
  expected = sorted(["d1.t%d" % i for i in range(N_TABLES)] + ["d1.p"])
  if names != expected:
    failures.append("table set %s, expected %s" % (names, expected))
  owners = set()
  for schema, name in tables:
    cur.execute("SELECT COLUMN_DEFAULT FROM information_schema.COLUMNS WHERE "
                "TABLE_SCHEMA='%s' AND TABLE_NAME='%s' AND COLUMN_NAME='owner'"
                % (schema, name))
    owner = int(cur.fetchall()[0][0])
    owners.add(owner)
    cur.execute("SELECT COUNT(*) FROM %s.%s WHERE owner <> %d" %
                (schema, name, owner))
    bad = cur.fetchall()[0][0]
    if bad:
      failures.append("%s.%s has %d rows of another table" % (schema, name,
                                                              bad))
    cur.execute("SELECT (SELECT COUNT(*) FROM %s.%s FORCE INDEX(PRIMARY)), "
                "(SELECT COUNT(*) FROM %s.%s FORCE INDEX(sk))" %
                (schema, name, schema, name))
    pk, sk = cur.fetchall()[0]
    if pk != sk:
      failures.append("%s.%s PK count %d != SK count %d" % (schema, name, pk,
                                                             sk))
    cur.execute("CHECK TABLE %s.%s" % (schema, name))
    for row in cur.fetchall():
      if row[2] == "status" and row[3] != "OK":
        failures.append("CHECK TABLE %s.%s: %s" % (schema, name, row[3]))
  if owners != set(range(N_TABLES)) | {PART_OWNER}:
    failures.append("table identities %s" % sorted(owners))

  # MyRocks dictionary must name exactly the server's tables (and partitions).
  cur.execute("SELECT DISTINCT TABLE_SCHEMA, TABLE_NAME FROM "
              "information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA IN ('d1','d2')"
              " ORDER BY 1, 2")
  rdb_names = sorted("%s.%s" % t for t in cur.fetchall())
  if rdb_names != expected:
    failures.append("ROCKSDB_DDL tables %s, expected %s" % (rdb_names,
                                                             expected))
  conn.close()
  return failures


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("--socket", required=True)
  parser.add_argument("--iterations", type=int, default=150)
  parser.add_argument("--seed", type=int, default=1)
  parser.add_argument("--writers", type=int, default=3)
  parser.add_argument("--stats", default=None,
                      help="file to write per-worker statement counts to")
  args = parser.parse_args()

  setup(args)
  it = args.iterations
  workers = [
      Truncater(args, "trunc1", it, args.seed + 1),
      Truncater(args, "trunc2", it, args.seed + 2),
      Swapper(args, "swap1", it, args.seed + 3),
      Swapper(args, "swap2", it, args.seed + 4),
      Mover(args, "move1", it // 2, args.seed + 5),
      PartTruncater(args, "ptrunc", it, args.seed + 6),
      Reader(args, "read1", it * 2, args.seed + 7),
      Maintenance(args, "maint", it, args.seed + 8),
  ]
  for w in range(args.writers):
    workers.append(Writer(args, "write%d" % w, it * 2, args.seed + 10 + w))
  for w in workers:
    w.start()
  for w in workers:
    w.join()

  if args.stats:
    with open(args.stats, "w") as f:
      for w in workers:
        f.write("%s ok=%d skipped=%s\n" % (w.name, w.ops, sorted(w.skipped.items())))
  errors = [e for w in workers for e in w.errors]
  for e in errors:
    print("ERROR: " + e)
  failures = check(args)
  for f in failures:
    print("ERROR: " + f)
  if errors or failures:
    sys.exit(1)
  print("concurrent load finished, all checks passed")


if __name__ == "__main__":
  main()
