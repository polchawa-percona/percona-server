#
# MyRocks copy of suite/innodb/include/alter_table_pk_no_sort.inc, used by
# rocksdb.innodb_port_innodb_index_debug. Changes: ENGINE=ROCKSDB, LOCK=NONE
# removed (MyRocks changes a primary key with ALGORITHM=COPY, which does not
# allow LOCK=NONE), every table is selected before it is dropped, and the
# descending index (not supported by MyRocks) becomes a reverse column family.
# EXPLAIN row estimates are masked (MyRocks statistics are approximate).
#
# skip sort for prefix change
# pk(o1(2)) to pk(o1(3))
create table t1(o1 varchar(10), primary key(o1(2))) engine = rocksdb;
insert into t1 values('abd'), ('acd');
alter table t1 drop primary key, add primary key(o1(3));
select * from t1;
drop table t1;

# pk(o1(2)) to pk(o1)
create table t1(o1 varchar(10), primary key(o1(2))) engine = rocksdb;
insert into t1 values('abd'), ('acd');
alter table t1 drop primary key, add primary key(o1);
select * from t1;
drop table t1;

# pk(o1(2)) to pk(o1(3),n1)
create table t1(o1 varchar(10), primary key(o1(2))) engine = rocksdb;
insert into t1 values('abd'), ('acd');
alter table t1 add n1 int not null, drop primary key, add primary key(o1(3), n1);
select * from t1;
drop table t1;

# pk(o1(2)) to pk(o1,n1)
create table t1(o1 varchar(10), primary key(o1(2))) engine = rocksdb;
insert into t1 values('abd'), ('acd');
alter table t1 add n1 int not null, drop primary key, add primary key(o1, n1);
select * from t1;
drop table t1;

# pk(o1(2)) to pk(o1(3), o2)
create table t1(o1 varchar(10), o2 int not null, primary key(o1(2))) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1(3), o2);
select * from t1;
drop table t1;

# pk(o1(2)) to pk(o1, o2)
create table t1(o1 varchar(10), o2 int not null, primary key(o1(2))) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1, o2);
select * from t1;
drop table t1;

# pk(o1(3)) to pk(o1(2))
create table t1(o1 varchar(10), primary key(o1(3))) engine = rocksdb;
insert into t1 values('abd'), ('acd');
alter table t1 drop primary key, add primary key(o1(2));
select * from t1;
drop table t1;

# pk(o1) to pk(o1(2))
create table t1(o1 varchar(10), primary key(o1)) engine = rocksdb;
insert into t1 values('abd'), ('acd');
alter table t1 drop primary key, add primary key(o1(2));
select * from t1;
drop table t1;

# pk(o1(3),o2) to pk(o1(2))
create table t1(o1 varchar(10), o2 int, primary key(o1(3), o2)) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 drop primary key, add primary key(o1(2));
select * from t1;
drop table t1;

# pk(o1,o2) to pk(o1(2))
create table t1(o1 varchar(10), o2 int, primary key(o1, o2)) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 drop primary key, add primary key(o1(2));
select * from t1;
drop table t1;

# pk(o1(3),o2) to pk(o1(2),n1)
create table t1(o1 varchar(10), o2 int, primary key(o1(3), o2)) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1(2),n1);
select * from t1;
drop table t1;

# pk(o1,o2) to pk(o1(2),n1)
create table t1(o1 varchar(10), o2 int, primary key(o1, o2)) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1(2),n1);
select * from t1;
drop table t1;

# pk(o1(3),o2) to pk(o1(3),n1)
create table t1(o1 varchar(10), o2 int, primary key(o1(3), o2)) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1(3),n1);
select * from t1;
drop table t1;

# pk(o1,o2) to pk(o1,n1)
create table t1(o1 varchar(10), o2 int, primary key(o1, o2)) engine = rocksdb;
insert into t1 values('abd', 1), ('acd', 2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1,n1);
select * from t1;
drop table t1;

# pk(o1,o2(3)) to pk(o1,o2(2))
create table t1(o1 int, o2 varchar(10), primary key(o1,o2(3))) engine = rocksdb;
insert into t1 values(1,'abd'), (2,'acd');
alter table t1 drop primary key, add primary key(o1,o2(2));
select * from t1;
drop table t1;

# pk(o1,o2) to pk(o1,o2(2))
create table t1(o1 int, o2 varchar(10), primary key(o1,o2)) engine = rocksdb;
insert into t1 values(1,'abd'), (2,'acd');
alter table t1 drop primary key, add primary key(o1,o2(2));
select * from t1;
drop table t1;

# pk(o1,o2(2)) to pk(o1,o2(3))
create table t1(o1 int, o2 varchar(10), primary key(o1,o2(2))) engine = rocksdb;
insert into t1 values(1, 'abd'), (2, 'acd');
alter table t1 drop primary key, add primary key(o1,o2(3));
select * from t1;
drop table t1;

# pk(o1,o2(2)) to pk(o1,o2)
create table t1(o1 int, o2 varchar(10), primary key(o1,o2(2))) engine = rocksdb;
insert into t1 values(1, 'abd'), (2, 'acd');
alter table t1 drop primary key, add primary key(o1,o2);
select * from t1;
drop table t1;

# pk(o1,o2(3),o3) to pk(o1,o2(2))
create table t1(o1 int, o2 varchar(10), o3 int, primary key(o1,o2(3),o3)) engine = rocksdb;
insert into t1 values(1, 'abd', 1), (2, 'acd', 2);
alter table t1 drop primary key, add primary key(o1,o2(2));
select * from t1;
drop table t1;

# pk(o1,o2,o3) to pk(o1,o2(2))
create table t1(o1 int, o2 varchar(10), o3 int, primary key(o1,o2,o3)) engine = rocksdb;
insert into t1 values(1, 'abd', 1), (2, 'acd', 2);
alter table t1 drop primary key, add primary key(o1,o2(2));
select * from t1;
drop table t1;

# pk(o1(3),o2(3)) to pk(o1(3),o2(2))
create table t1(o1 varchar(10), o2 varchar(10), primary key(o1(3),o2(3))) engine = rocksdb;
insert into t1 values('abd', 'acd'), ('acd', 'abd');
alter table t1 drop primary key, add primary key(o1(3),o2(2));
select * from t1;
drop table t1;

# pk(o1,o2) to pk(o1,o2(2))
create table t1(o1 varchar(10), o2 varchar(10), primary key(o1,o2)) engine = rocksdb;
insert into t1 values('abd', 'acd'), ('acd', 'abd');
alter table t1 drop primary key, add primary key(o1,o2(2));
select * from t1;
drop table t1;

# pk(o1(3),o2(2)) to pk(o1(3),o2(3))
create table t1(o1 varchar(10), o2 varchar(10), primary key(o1(3),o2(2))) engine = rocksdb;
insert into t1 values('abd', 'acd'), ('acd', 'abd');
alter table t1 drop primary key, add primary key(o1(3),o2(3));
select * from t1;
drop table t1;

# pk(o1,o2(2)) to pk(o1,o2)
create table t1(o1 varchar(10), o2 varchar(10), primary key(o1,o2(2))) engine = rocksdb;
insert into t1 values('abd', 'acd'), ('acd', 'abd');
alter table t1 drop primary key, add primary key(o1,o2);
select * from t1;
drop table t1;

# pk(o1(3),o2,o3(2)) to pk(o1(3),o2,o3(3))
create table t1(o1 varchar(10), o2 int, o3 varchar(10), primary key(o1(3),o2,o3(2))) engine = rocksdb;
insert into t1 values('abd', 1, 'acd'), ('acd', 2, 'abd');
alter table t1 drop primary key, add primary key(o1(3),o2,o3(3));
select * from t1;
drop table t1;

# pk(o1,o2,o3(2)) to pk(o1,o2,o3)
create table t1(o1 varchar(10), o2 int, o3 varchar(10), primary key(o1,o2,o3(2))) engine = rocksdb;
insert into t1 values('abd', 1, 'acd'), ('acd', 2, 'abd');
alter table t1 drop primary key, add primary key(o1,o2,o3);
select * from t1;
drop table t1;

# pk(o1(3),o2,o3(3)) to pk(o1(3),o2,o3(2))
create table t1(o1 varchar(10), o2 int, o3 varchar(10), primary key(o1(3),o2,o3(3))) engine = rocksdb;
insert into t1 values('abd', 1, 'acd'), ('acd', 2, 'abd');
alter table t1 drop primary key, add primary key(o1(3),o2,o3(2));
select * from t1;
drop table t1;

# pk(o1,o2,o3) to pk(o1,o2,o3(2))
create table t1(o1 varchar(10), o2 int, o3 varchar(10), primary key(o1,o2,o3(3))) engine = rocksdb;
insert into t1 values('abd', 1, 'acd'), ('acd', 2, 'abd');
alter table t1 drop primary key, add primary key(o1,o2,o3(2));
select * from t1;
drop table t1;

# skip sort for adding existing columns/newly added columns, dropping pk columns at the end.
# pk(o1) to pk(o1,o2)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 drop primary key, add primary key(o1,o2);
select * from t1;
drop table t1;

# pk(o1) to pk(o1,n1)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1,n1);
select * from t1;
drop table t1;

# pk(o1) to pk(n1,o1)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, drop primary key, add primary key(n1,o1);
select * from t1;
drop table t1;

# pk(o1) to pk(n1,o1,n2)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, add n2 int not null, drop primary key, add primary key(n1,o1,n2);
select * from t1;
drop table t1;

# pk(o1) to pk(n1,n2,o1)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, add n2 int not null, drop primary key, add primary key(n1,n2,o1);
select * from t1;
drop table t1;

# pk(o1) to pk(o1,n1,n2)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, add n2 int not null, drop primary key, add primary key(o1,n1,n2);
select * from t1;
drop table t1;

# pk(o1) to pk(o1,o2,n1)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1,o2,n1);
select * from t1;
drop table t1;

# pk(o1) to pk(o1,n1,o2)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, drop primary key, add primary key(o1,n1,o2);
select * from t1;
drop table t1;

# pk(o1) to pk(n1,o1,o2)
create table t1(o1 int, o2 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1),(2,2);
alter table t1 add n1 int not null, drop primary key, add primary key(n1,o1,o2);
select * from t1;
drop table t1;

# pk(o1) to pk(o1,o2,o3)
create table t1(o1 int, o2 int not null, o3 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1,2),(2,2,1);
alter table t1 drop primary key, add primary key(o1,o2,o3);
select * from t1;
drop table t1;

# pk(o1) to pk(o1,o3,o2)
create table t1(o1 int, o2 int not null, o3 int not null, primary key(o1)) engine = rocksdb;
insert into t1 values(1,1,2),(2,2,1);
alter table t1 drop primary key, add primary key(o1,o3,o2);
select * from t1;
drop table t1;

# pk(o1,o2,o3) to pk(o1,o2)
create table t1(o1 int, o2 int, o3 int, primary key(o1,o2,o3)) engine = rocksdb;
insert into t1 values(1,1,2),(2,2,1);
alter table t1 drop primary key, add primary key(o1,o2);
select * from t1;
drop table t1;

# pk(o1,o2,o3) to pk(o1,o2,o3,o4)
create table t1(o1 int, o2 int, o3 int, o4 int not null, primary key(o1,o2,o3)) engine = rocksdb;
insert into t1 values(1,1,2,2),(2,2,1,1);
alter table t1 add n1 int not null, drop primary key, add primary key(o1,o2,o3,o4);
select * from t1;
drop table t1;

# pk(o1,o2,o3) to pk(o1,o2,n1)
create table t1(o1 int, o2 int, o3 int, primary key(o1,o2,o3)) engine = rocksdb;
insert into t1 values(1,1,2),(2,2,1);
alter table t1 add n1 int not null, drop primary key, add primary key(o1,o2,n1);
select * from t1;
drop table t1;

# pk(o1,o2,o3) to pk(o1,n1,o2)
create table t1(o1 int, o2 int, o3 int, primary key(o1,o2,o3)) engine = rocksdb;
insert into t1 values(1,1,2),(2,2,1);
alter table t1 add n1 int not null, drop primary key, add primary key(o1,n1,o2);
select * from t1;
drop table t1;

# pk(o1,o2,o3) to pk(o1)
create table t1(o1 int, o2 int, o3 int, primary key(o1,o2,o3)) engine = rocksdb;
insert into t1 values(1,1,2),(2,2,1);
alter table t1 drop primary key, add primary key(o1);
select * from t1;
drop table t1;

# pk(o1) to pk(o1,o2)
CREATE TABLE t1(o1 int, o2 int, o3 int, PRIMARY KEY(o1), KEY(o2)) ENGINE=ROCKSDB;
INSERT INTO t1 VALUES(1,1,2),(2,2,1);

--replace_column 10 #
EXPLAIN SELECT * FROM t1 ORDER BY o2;
SELECT * FROM t1 ORDER BY o2;

# MyRocks has no descending indexes; a reverse column family is the equivalent.
--error ER_CHECK_NOT_IMPLEMENTED
ALTER TABLE t1 DROP INDEX o2, ADD INDEX (o2 DESC);
ALTER TABLE t1 DROP INDEX o2, ADD INDEX o2 (o2) COMMENT 'rev:cf_o2';

CHECK TABLE t1;
SELECT * FROM t1;
--replace_column 10 #
EXPLAIN SELECT * FROM t1 ORDER BY o2;
SELECT * FROM t1 ORDER BY o2;

ALTER TABLE t1 DROP INDEX o2, ADD INDEX (o2 ASC);

CHECK TABLE t1;
SELECT * FROM t1;
--replace_column 10 #
EXPLAIN SELECT * FROM t1 ORDER BY o2;
SELECT * FROM t1 ORDER BY o2;

DROP TABLE t1;
