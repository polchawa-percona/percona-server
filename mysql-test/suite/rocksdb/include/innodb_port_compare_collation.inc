# Compare a MyRocks table with an InnoDB twin for one character set and
# collation. Used by tests ported from suite/innodb.
#
# Parameters:
#   $cs      character set
#   $coll    collation
#   $values  VALUES list of strings to store, e.g. ('a'),('a '),(NULL)
#   $len     optional VARCHAR length (default 10)
#   $cc_engine_under_test  optional, default ROCKSDB. Only used to produce
#            the expected result of a disabled test that cannot run on MyRocks
#            yet (the result is then edited back to ENGINE=ROCKSDB).
#
# Both engines get a table with a full-column secondary index (kc) and a
# 2-character prefix index (kp), plus a table with a unique key. The include
# prints one line per check:
# - which inserts the unique key rejects as duplicates (must match InnoDB);
# - each MyRocks secondary index against the MyRocks primary key
#   (innodb_port_check_index.inc);
# - equality, LIKE-prefix, greater-than and BETWEEN lookups through each
#   MyRocks index against the same lookups through the same InnoDB index.
#   Both sides use the same plan on purpose (x scanned without an index,
#   STRAIGHT_JOIN, y probed through the forced index): for collations with
#   contractions or expansions (e.g. WL#5624 collations) a prefix-index
#   lookup can miss rows that are equal to the probe, on both engines, so
#   comparing with an unindexed InnoDB scan would flag a server limitation
#   instead of an engine difference.

--echo # $coll
--let $_cc_len = $len
if (!$_cc_len)
{
  --let $_cc_len = 10
}
--let $engine_i = 1
while ($engine_i <= 2)
{
  --let $eng = ROCKSDB
  if ($cc_engine_under_test)
  {
    --let $eng = $cc_engine_under_test
  }
  --let $tbl = r
  if ($engine_i == 2)
  {
    --let $eng = InnoDB
    --let $tbl = i
  }
  --eval CREATE TABLE $tbl (id INT AUTO_INCREMENT PRIMARY KEY, c VARCHAR($_cc_len) CHARACTER SET $cs COLLATE $coll, KEY kc (c), KEY kp (c(2))) ENGINE=$eng
  --eval CREATE TABLE u$tbl (c VARCHAR($_cc_len) CHARACTER SET $cs COLLATE $coll, UNIQUE KEY uc (c)) ENGINE=$eng
  --eval INSERT INTO $tbl (c) VALUES $values
  # Insert one row at a time into the unique table and remember which
  # inserts were rejected as duplicates.
  --eval CREATE TEMPORARY TABLE dup_$tbl (id INT, rejected INT) ENGINE=InnoDB
  --let $n = `SELECT MAX(id) FROM $tbl`
  --let $k = 1
  --disable_query_log
  while ($k <= $n)
  {
    --error 0,ER_DUP_ENTRY
    --eval INSERT INTO u$tbl SELECT c FROM $tbl WHERE id = $k
    --let $errno = $mysql_errno
    --eval INSERT INTO dup_$tbl VALUES ($k, $errno != 0)
    --inc $k
  }
  --enable_query_log
  --inc $engine_i
}

# Unique-key decisions must match InnoDB's.
--let $diff = `SELECT COUNT(*) FROM dup_r JOIN dup_i USING (id) WHERE dup_r.rejected <> dup_i.rejected`
--let $rejected = `SELECT GROUP_CONCAT(HEX(r.c) ORDER BY r.id) FROM dup_r JOIN r USING (id) WHERE dup_r.rejected`
if ($diff == 0)
{
  --echo #   unique-key duplicates same as InnoDB (rejected: $rejected)
}
if ($diff != 0)
{
  --echo #   MISMATCH unique-key duplicates differ from InnoDB in $diff rows
  SELECT id, HEX(c), dup_r.rejected AS rocksdb, dup_i.rejected AS innodb FROM dup_r JOIN dup_i USING (id) JOIN r USING (id) WHERE dup_r.rejected <> dup_i.rejected;
}

# MyRocks secondary indexes against the MyRocks primary key.
--let $cpi_table = r
--let $cpi_pk = id
--let $cpi_index = kc
--let $cpi_cols = c
--source suite/rocksdb/include/innodb_port_check_index.inc
--let $cpi_index = kp
--source suite/rocksdb/include/innodb_port_check_index.inc
--let $cpi_table = ur
--let $cpi_pk = c
--let $cpi_index = uc
--let $cpi_cols = c
--source suite/rocksdb/include/innodb_port_check_index.inc

# Lookups through each MyRocks index against InnoDB. Each probe value is a
# stored string; the result is the sorted list of matching ids.
--let $q = 1
while ($q <= 6)
{
  if ($q == 1)
  {
    --let $where = y.c = x.c
    --let $idx = kc
  }
  if ($q == 2)
  {
    --let $idx = kp
  }
  if ($q == 3)
  {
    --let $where = y.c LIKE CONCAT(LEFT(x.c, 1), '%')
    --let $idx = kc
  }
  if ($q == 4)
  {
    --let $idx = kp
  }
  if ($q == 5)
  {
    --let $where = y.c > x.c
    --let $idx = kc
  }
  if ($q == 6)
  {
    --let $where = y.c BETWEEN x.c AND CONCAT(x.c, 'z')
    --let $idx = kp
  }
  # SELECT ... INTO, so that an error during the scan fails the test with
  # its message instead of an empty result.
  --disable_query_log
  --eval SELECT SUM(CRC32(CONCAT(x.id, ':', y.id))) INTO @via_r FROM r AS x IGNORE INDEX(kc, kp) STRAIGHT_JOIN r AS y FORCE INDEX($idx) ON $where
  --eval SELECT SUM(CRC32(CONCAT(x.id, ':', y.id))) INTO @via_i FROM i AS x IGNORE INDEX(kc, kp) STRAIGHT_JOIN i AS y FORCE INDEX($idx) ON $where
  --enable_query_log
  --let $via_r = `SELECT @via_r`
  --let $via_i = `SELECT @via_i`
  if ($via_r == $via_i)
  {
    --echo #   $idx: $where same rows as InnoDB
  }
  if ($via_r != $via_i)
  {
    --echo #   MISMATCH $idx: $where differs from InnoDB
    --eval SELECT x.id, HEX(x.c), GROUP_CONCAT(y.id ORDER BY y.id) FROM r AS x IGNORE INDEX(kc, kp) STRAIGHT_JOIN r AS y FORCE INDEX($idx) ON $where GROUP BY x.id ORDER BY x.id
    --eval SELECT x.id, HEX(x.c), GROUP_CONCAT(y.id ORDER BY y.id) FROM i AS x IGNORE INDEX(kc, kp) STRAIGHT_JOIN i AS y FORCE INDEX($idx) ON $where GROUP BY x.id ORDER BY x.id
  }
  --inc $q
}

DROP TABLE r, i, ur, ui;
DROP TEMPORARY TABLE dup_r, dup_i;
