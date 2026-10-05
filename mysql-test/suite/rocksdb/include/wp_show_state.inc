# Print the visible state of test.t1 through several access paths.
# Used by wp_prepared_visibility.test and wp_prepared_crash_recovery.test.
# Every query is a plain (non-locking) read, so inside a REPEATABLE READ
# transaction it reads the transaction's snapshot.
#
# Optional: $wp_label  - text printed before the state.
if ($wp_label)
{
  --echo # $wp_label
}
SELECT COUNT(*), SUM(pk), SUM(a) FROM t1;
SELECT pk, a, b FROM t1 ORDER BY pk;
# Covering read through the secondary index kb (no primary key lookup).
SELECT pk, b FROM t1 FORCE INDEX (kb) ORDER BY b, pk;
# Unique secondary index, point lookups for an old and a new key.
SELECT pk, a FROM t1 FORCE INDEX (ua) WHERE a IN (30, 110);
# Range scan across the rows the other transaction changes.
SELECT pk, a, b FROM t1 FORCE INDEX (PRIMARY) WHERE pk BETWEEN 8 AND 12 ORDER BY pk;
# Point lookups by primary key.
SELECT pk, b FROM t1 WHERE pk = 2;
SELECT pk, b FROM t1 WHERE pk = 11;
--let $wp_label=
