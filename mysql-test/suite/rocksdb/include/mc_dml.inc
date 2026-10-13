# One batch of DML over all mc tables, run by the current connection without
# committing. $mc_round (1, 2, ...) shifts the keys so that every round
# changes different rows. Covers INSERT, UPDATE of primary and secondary key
# values (including a change of letter case only, which keeps the key and
# changes the unpack info), DELETE, REPLACE and INSERT ... ON DUPLICATE KEY
# UPDATE.
# Every UPDATE and DELETE names the full primary key, so the batch also runs
# in a REPEATABLE READ multi-statement transaction (the gap-lock rule refuses
# other locking reads there). The hidden-primary-key table has no unique key
# to name, so at REPEATABLE READ it only gets inserts.
--disable_query_log
eval SET @r = $mc_round;
# Primary key covering all columns: delete, reinsert, change the key.
DELETE FROM mc.t_pk WHERE id = 10 * @r;
INSERT INTO mc.t_pk VALUES (10 * @r);
DELETE FROM mc.t_pk WHERE id = 10 * @r + 1;
UPDATE mc.t_pk SET id = id + 1000 WHERE id = 10 * @r + 2;
REPLACE INTO mc.t_pk VALUES (10 * @r + 3), (5000 + @r);
INSERT INTO mc.t_pk VALUES (10 * @r + 4) ON DUPLICATE KEY UPDATE id = id + 2000;
# Hidden primary key.
if (`SELECT @@transaction_isolation = 'READ-COMMITTED'`) {
  UPDATE mc.t_hpk SET b = UPPER(b) WHERE a = @r;
  UPDATE mc.t_hpk SET a = a + 100 WHERE a = @r + 10;
  DELETE FROM mc.t_hpk WHERE a = @r + 20 LIMIT 2;
}
INSERT INTO mc.t_hpk VALUES (@r + 20, 'reinserted'), (@r + 20, 'reinserted');
# Several secondary keys.
UPDATE mc.t_multi SET v = UPPER(v) WHERE id = @r;
UPDATE mc.t_multi SET v = CONCAT('moved', @r), c = CONCAT('c', @r)
  WHERE id = @r + 20;
UPDATE mc.t_multi SET u = u + 100000 WHERE id = @r + 40;
DELETE FROM mc.t_multi WHERE id = @r + 60;
INSERT INTO mc.t_multi VALUES (@r + 60, (@r + 60) * 10, 'back', 'back again');
REPLACE INTO mc.t_multi VALUES (@r + 80, @r * 10 + 7, 'replaced', 'r');
INSERT INTO mc.t_multi VALUES (@r + 100, 0, 'x', 'x')
  ON DUPLICATE KEY UPDATE u = u + 200000, v = 'dup';
INSERT INTO mc.t_multi VALUES (1000 + @r, 3000000 + @r, 'new', 'new row');
# Own and reverse column families.
UPDATE mc.t_cf SET b = CONCAT('B', @r) WHERE id = @r * 5;
UPDATE mc.t_cf SET b = CONCAT('b', @r) WHERE id = @r * 5 + 1;
UPDATE mc.t_cf SET a = a + 1 WHERE id = @r + 150;
DELETE FROM mc.t_cf WHERE id = @r + 120;
REPLACE INTO mc.t_cf VALUES (@r + 130, (@r + 130) * 10, 'replaced');
# Partial index: rows move between materialized groups, values change inside
# a group. Group 1 stays below the threshold (read through the primary key);
# growing it in an uncommitted transaction hits PS-11783 (see
# partial_index_materialize_uncommitted.test).
UPDATE mc.t_part SET v = v + 100 WHERE g = 2 + (@r + 50) % 10 AND id = @r + 50;
UPDATE mc.t_part SET g = 2 + (@r + 1) % 10 WHERE g = 2 + @r % 10 AND id = @r + 70;
INSERT INTO mc.t_part VALUES (2 + @r % 10, 1000 + @r, @r), (2 + @r % 10, 2000 + @r, @r);
DELETE FROM mc.t_part WHERE g = 2 + (@r + 90) % 10 AND id = @r + 90;
--enable_query_log
