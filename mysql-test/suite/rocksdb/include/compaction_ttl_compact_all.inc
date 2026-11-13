# Compact every column family used by the compaction_ttl_* tables.
SET GLOBAL rocksdb_compact_cf = 'cft';
SET GLOBAL rocksdb_compact_cf = 'rev:cft2';
SET GLOBAL rocksdb_compact_cf = 'cfp0';
SET GLOBAL rocksdb_compact_cf = 'cfp1';
