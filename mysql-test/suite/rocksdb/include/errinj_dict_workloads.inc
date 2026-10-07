#
# Runs errinj_loop.inc over DDL workloads with class $errinj_class and status
# $errinj_status. $errinj_dict_group selects the workloads: all, create_drop,
# rename, truncate, drop_index, copy_alter or instant. Setting
# $errinj_dict_skip_crash_windows leaves out the workloads whose aborts hit
# the known crash windows of DROP of a partitioned table (PS-11666) and COPY
# ALTER (PS-11681). Needs errinj_dict_setup.inc.
#
--let $g = $errinj_dict_group

--let $errinj_reset = CALL errinj_dict_reset(0)
if (`SELECT '$g' IN ('all', 'create_drop')`) {
  --let $errinj_tables = t2
  --let $errinj_stmt = CREATE TABLE t3 (pk INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=ROCKSDB
  --let $errinj_label = $errinj_class/$errinj_status CREATE TABLE
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = CREATE TABLE tp (pk INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=ROCKSDB PARTITION BY HASH (pk) PARTITIONS 3
  --let $errinj_label = $errinj_class/$errinj_status CREATE partitioned TABLE
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_stmt = CREATE TABLE tc (pk INT NOT NULL, a INT NOT NULL, PRIMARY KEY (pk) COMMENT 'cfname=cf_errinj', KEY ka (a) COMMENT 'cfname=rev:cf_errinj2') ENGINE=ROCKSDB
  --let $errinj_label = $errinj_class/$errinj_status CREATE TABLE in new column families
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_tables =
  --let $errinj_stmt = DROP TABLE t2
  --let $errinj_label = $errinj_class/$errinj_status DROP TABLE
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_after_restart = SET GLOBAL rocksdb_enable_delete_range_for_drop_index = ON
  eval $errinj_after_restart;
  --let $errinj_label = $errinj_class/$errinj_status DROP TABLE with delete range
  --source suite/rocksdb/include/errinj_loop.inc
  SET GLOBAL rocksdb_enable_delete_range_for_drop_index = DEFAULT;
  --let $errinj_after_restart =

  --let $errinj_reset = CALL errinj_dict_reset(1)
  if (!$errinj_dict_skip_crash_windows) {
    --let $errinj_stmt = DROP TABLE tp
    --let $errinj_label = $errinj_class/$errinj_status DROP partitioned TABLE
    --source suite/rocksdb/include/errinj_loop.inc
  }

  --let $errinj_reset = CALL errinj_dict_reset(2)
  --let $errinj_stmt = DROP TABLE tc
  --let $errinj_label = $errinj_class/$errinj_status DROP TABLE in own column families
  --source suite/rocksdb/include/errinj_loop.inc
  --let $errinj_reset = CALL errinj_dict_reset(0)
}

if (`SELECT '$g' IN ('all', 'rename')`) {
  --let $errinj_tables =
  --let $errinj_stmt = RENAME TABLE t2 TO t3
  --let $errinj_label = $errinj_class/$errinj_status RENAME TABLE
  --source suite/rocksdb/include/errinj_loop.inc
}

if (`SELECT '$g' IN ('all', 'truncate')`) {
  --let $errinj_tables = t2
  --let $errinj_stmt = TRUNCATE TABLE t2
  --let $errinj_label = $errinj_class/$errinj_status TRUNCATE
  --source suite/rocksdb/include/errinj_loop.inc
}

if (`SELECT '$g' IN ('all', 'drop_index')`) {
  --let $errinj_tables = t2
  --let $errinj_stmt = ALTER TABLE t2 DROP INDEX kb, ALGORITHM=INPLACE
  --let $errinj_label = $errinj_class/$errinj_status INPLACE DROP INDEX
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_after_restart = SET GLOBAL rocksdb_enable_instant_ddl = ON, GLOBAL rocksdb_enable_instant_ddl_for_drop_index_changes = ON
  eval $errinj_after_restart;
  --let $errinj_stmt = ALTER TABLE t2 DROP INDEX kd
  --let $errinj_label = $errinj_class/$errinj_status DROP INDEX with instant DDL on
  --source suite/rocksdb/include/errinj_loop.inc
  SET GLOBAL rocksdb_enable_instant_ddl_for_drop_index_changes = DEFAULT;
  SET GLOBAL rocksdb_enable_instant_ddl = DEFAULT;
  --let $errinj_after_restart =
}

if (`SELECT '$g' IN ('all', 'copy_alter') AND '$errinj_dict_skip_crash_windows' IN ('', '0')`) {
  --let $errinj_tables = t2
  --let $errinj_stmt = ALTER TABLE t2 ADD COLUMN e INT NOT NULL DEFAULT 5, ALGORITHM=COPY
  --let $errinj_label = $errinj_class/$errinj_status COPY ALTER
  --source suite/rocksdb/include/errinj_loop.inc

  --let $errinj_reset = CALL errinj_dict_reset(1)
  --let $errinj_tables = t2,tp
  --let $errinj_stmt = ALTER TABLE tp ADD PARTITION PARTITIONS 1
  --let $errinj_label = $errinj_class/$errinj_status ADD PARTITION
  --source suite/rocksdb/include/errinj_loop.inc
  --let $errinj_reset = CALL errinj_dict_reset(0)
}

if (`SELECT '$g' IN ('all', 'instant')`) {
  --let $errinj_tables = t2
  --let $errinj_after_restart = SET GLOBAL rocksdb_enable_instant_ddl = ON, GLOBAL rocksdb_enable_instant_ddl_for_append_column = ON
  eval $errinj_after_restart;
  --let $errinj_stmt = ALTER TABLE t2 ADD COLUMN e INT NOT NULL DEFAULT 7, ALGORITHM=INSTANT
  --let $errinj_label = $errinj_class/$errinj_status INSTANT ADD COLUMN
  --source suite/rocksdb/include/errinj_loop.inc
  SET GLOBAL rocksdb_enable_instant_ddl_for_append_column = DEFAULT;
  SET GLOBAL rocksdb_enable_instant_ddl = DEFAULT;
  --let $errinj_after_restart =
}
