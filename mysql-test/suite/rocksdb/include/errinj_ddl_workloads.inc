#
# Runs errinj_loop.inc over the DDL workloads for one class and status.
# Needs errinj_ddl_setup.inc, $errinj_class and $errinj_status.
# $errinj_bulk_load: 0 or unset runs every workload except bulk load,
# 1 runs only bulk load, 2 runs all of them.
#

--let $errinj_tables = t2

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = ALTER TABLE t2 ADD INDEX kd (d), ALGORITHM=INPLACE
  --let $errinj_label = $errinj_class/$errinj_status INPLACE ADD INDEX
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = ALTER TABLE t2 ADD UNIQUE INDEX ua (a), ALGORITHM=INPLACE
  --let $errinj_label = $errinj_class/$errinj_status INPLACE ADD UNIQUE INDEX
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = ALTER TABLE t2 DROP INDEX kb, ALGORITHM=INPLACE
  --let $errinj_label = $errinj_class/$errinj_status INPLACE DROP INDEX
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = ALTER TABLE t2 ADD COLUMN e INT NOT NULL DEFAULT 5, ADD INDEX ke (e), ALGORITHM=COPY
  --let $errinj_label = $errinj_class/$errinj_status COPY ALTER
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = TRUNCATE TABLE t2
  --let $errinj_label = $errinj_class/$errinj_status TRUNCATE
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load) {
  --let $errinj_stmt = CALL errinj_bulk_load()
  --let $errinj_label = $errinj_class/$errinj_status bulk load
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = OPTIMIZE TABLE t2
  --let $errinj_label = $errinj_class/$errinj_status OPTIMIZE TABLE
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = ANALYZE TABLE t2
  --let $errinj_label = $errinj_class/$errinj_status ANALYZE TABLE
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_stmt = RENAME TABLE t2 TO t2_renamed, t2_renamed TO t2
  --let $errinj_label = $errinj_class/$errinj_status RENAME TABLE
  --source suite/rocksdb/include/errinj_loop.inc
}

if ($errinj_bulk_load != 1) {
  --let $errinj_tables =
  --let $errinj_stmt = DROP TABLE t2
  --let $errinj_label = $errinj_class/$errinj_status DROP TABLE
  --source suite/rocksdb/include/errinj_loop.inc
}
