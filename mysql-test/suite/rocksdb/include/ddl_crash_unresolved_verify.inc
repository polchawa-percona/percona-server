# Check that table $vdb.$vt is usable and matches the server's
# data dictionary: opening it resolves a pending inplace ALTER TABLE record.
# Used by ddl_crash_unresolved.inc.

--eval SHOW CREATE TABLE $vdb.$vt
--eval SELECT PARTITION_NAME, INDEX_NAME FROM information_schema.ROCKSDB_DDL WHERE TABLE_SCHEMA = '$vdb' AND TABLE_NAME = '$vt' ORDER BY PARTITION_NAME, INDEX_NAME
--eval SELECT * FROM $vdb.$vt ORDER BY id
--error 0,ER_KEY_DOES_NOT_EXITS
--eval SELECT id, a FROM $vdb.$vt FORCE INDEX(ka) WHERE a > 0 ORDER BY a
--error 0,ER_KEY_DOES_NOT_EXITS
--eval SELECT id, b FROM $vdb.$vt FORCE INDEX(kb) WHERE b > 0 ORDER BY b
--eval INSERT INTO $vdb.$vt VALUES (4,40,40)
--eval UPDATE $vdb.$vt SET a = a + 100, b = b + 100 WHERE id = 1
--error 0,ER_KEY_DOES_NOT_EXITS
--eval SELECT id, a FROM $vdb.$vt FORCE INDEX(ka) WHERE a > 0 ORDER BY a
--error 0,ER_KEY_DOES_NOT_EXITS
--eval SELECT id, b FROM $vdb.$vt FORCE INDEX(kb) WHERE b > 0 ORDER BY b
--eval CHECK TABLE $vdb.$vt
