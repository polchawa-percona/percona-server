# Release the compaction held by compaction_ttl_pause.inc and wait for it.
SET GLOBAL debug = '-d,rocksdb_pause_compaction';
connection con_compact;
# rocksdb_compact_lzero_now fails if an automatic compaction takes the L0
# files first (PS-11747).
--error 0,ER_WRONG_VALUE_FOR_VAR
--reap
connection default;
