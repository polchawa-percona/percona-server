# Creates and fills one table of a given shape.
#   $mc3_t     table name, schema-qualified
#   $mc3_shape live: PK, reverse-CF SK on a utf8mb4_0900_ai_ci column (unpack
#                    info), unique SK, BLOB
#              hid:  hidden PK, SK on a latin1 non-binary collation
#              pidx: partial secondary index next to a full one
#   $mc3_rows  rows to insert; $mc3_off key offset (default 0)
# The checksum column list for each shape is in mc3_snap.inc.

if (!$mc3_off) {
  --let $mc3_off = 0
}
--disable_query_log
if ($mc3_shape == live) {
  eval CREATE TABLE $mc3_t (id INT PRIMARY KEY COMMENT 'cfname=mc3_cf',
    a VARCHAR(64) COLLATE utf8mb4_0900_ai_ci, u INT, b BLOB,
    KEY ka (a) COMMENT 'cfname=rev:mc3_rcf',
    UNIQUE KEY ku (u) COMMENT 'cfname=mc3_cf') ENGINE=ROCKSDB;
}
if ($mc3_shape == hid) {
  eval CREATE TABLE $mc3_t (a INT, c VARCHAR(32) COLLATE latin1_swedish_ci,
    KEY kc (c) COMMENT 'cfname=mc3_cf') ENGINE=ROCKSDB;
}
if ($mc3_shape == pidx) {
  eval CREATE TABLE $mc3_t (i VARCHAR(16), j INT, k INT, l VARCHAR(16),
    PRIMARY KEY (i, j) COMMENT 'cfname=mc3_cf',
    KEY ik1 (i, k) COMMENT 'cfname=mc3_cf;partial_group_keyparts=1;partial_group_threshold=5',
    KEY ik2 (k, i) COMMENT 'cfname=rev:mc3_rcf') ENGINE=ROCKSDB;
}
--source suite/rocksdb/include/mc3_fill.inc
--enable_query_log
