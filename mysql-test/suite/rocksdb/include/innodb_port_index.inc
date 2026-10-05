# MyRocks port of mysql-test/include/innodb-index.inc, which
# suite/innodb/t/innodb-index_ucs2.test uses.
#
# Parameters:
#   $charset    table character set
#   $collation  optional table collation
#
# Changes from the InnoDB original:
# - ENGINE=ROCKSDB.
# - EXPLAIN row estimates and "filtered" are masked: MyRocks estimates differ
#   from InnoDB's and are not what this test is about.
# - The duplicate-key message for ADD UNIQUE INDEX names MyRocks's
#   temporary table; the name is masked.
# - After each set of indexes is added, every secondary index is checked
#   against the primary key with innodb_port_check_index.inc.

--let $_ipi_coll =
if ($collation)
{
  --let $_ipi_coll = COLLATE $collation
}

--eval create table t1(a int not null, b int, c char(10), d varchar(20), primary key (a)) engine = rocksdb default charset=$charset $_ipi_coll
insert into t1 values (1,1,'ab','ab'),(2,2,'ac','ac'),(3,2,'ad','ad'),(4,4,'afe','afe');
commit;
# MyRocks names the rebuilt temporary table in the duplicate-key message
# ('#sql-..._a.b') where InnoDB says 't1.b'.
--replace_regex /#sql-[0-9a-f]+_[0-9a-f]+/#sql-tmp/
--error ER_DUP_ENTRY
alter table t1 add unique index (b);
insert into t1 values(8,9,'fff','fff');
select * from t1;
show create table t1;
alter table t1 add index (b);
insert into t1 values(10,10,'kkk','iii');
-- disable_query_log
-- disable_result_log
analyze table t1;
-- enable_result_log
-- enable_query_log
select * from t1;
select * from t1 force index(b) order by b;
--replace_column 10 # 11 #
explain select * from t1 force index(b) order by b;
show create table t1;
alter table t1 add unique index (c), add index (d);
insert into t1 values(11,11,'aaa','mmm');
-- disable_query_log
-- disable_result_log
analyze table t1;
-- enable_result_log
-- enable_query_log
select * from t1;
select * from t1 force index(b) order by b;
select * from t1 force index(c) order by c;
select * from t1 force index(d) order by d;
--replace_column 10 # 11 #
explain select * from t1 force index(b) order by b;
--replace_column 10 # 11 #
explain select * from t1 force index(c) order by c;
--replace_column 10 # 11 #
explain select * from t1 force index(d) order by d;
show create table t1;
check table t1;

--let $cpi_table = t1
--let $cpi_pk = a
--let $cpi_index = b
--let $cpi_cols = b
--source suite/rocksdb/include/innodb_port_check_index.inc
--let $cpi_index = c
--let $cpi_cols = c
--source suite/rocksdb/include/innodb_port_check_index.inc
--let $cpi_index = d
--let $cpi_cols = d
--source suite/rocksdb/include/innodb_port_check_index.inc

drop table t1;
--let $collation =
