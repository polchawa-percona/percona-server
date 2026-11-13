#
# Records the state of table test.$tbl after partition DDL step $step:
# row count and checksum read through the primary key, the same read through
# the covering secondary index kb, and the number of rows in each
# (sub)partition. Prints the row count, the per-partition distribution and
# whether the secondary index agrees with the primary key.
#
# Parameters: $tbl, $step, $run (engine/mode tag), $ptype.
#
--disable_query_log
eval CALL test.ddl3_parts('$tbl', @parts);
# Plain reads into variables: INSERT ... SELECT from a MyRocks table would
# be a locking read, which the gap-lock rule refuses at REPEATABLE READ.
eval SELECT COUNT(*), BIT_XOR(CRC32(CONCAT_WS(':', a, b, c))), BIT_XOR(CRC32(CONCAT_WS(':', a, b)))
  INTO @cnt, @crc, @pk_ab FROM test.$tbl FORCE INDEX (PRIMARY);
eval SELECT COUNT(*), BIT_XOR(CRC32(CONCAT_WS(':', a, b)))
  INTO @sk_cnt, @sk_ab FROM test.$tbl FORCE INDEX (kb);
eval INSERT INTO test.mlog (run, ptype, step, cnt, crc, pk_ab, sk_ab, sk_cnt, parts)
  VALUES ('$run', '$ptype', '$step', @cnt, @crc, @pk_ab, @sk_ab, @sk_cnt, @parts);
SELECT step, cnt AS row_count, (sk_cnt = cnt AND sk_ab <=> pk_ab) AS sk_matches_pk, parts
  FROM test.mlog WHERE id = LAST_INSERT_ID();
--enable_query_log
