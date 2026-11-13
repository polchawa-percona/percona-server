#
# WL#6737: Integrating InnoDB temporary tables with MySQL Optimizer.
#
# Copy of suite/innodb/include/query_workload_itt.inc for
# rocksdb.innodb_port_optimizer_temporary_table. Changes:
#   - the EXPLAIN FORMAT=JSON statements still run, but their output (cost
#     estimates that depend on the engine's statistics) is not recorded
#   - the multi-table UPDATE is skipped when $skip_update is set, instead of
#     when innodb_read_only is set; t2 is shown after the UPDATE
#

SET @optimizer_switch_saved= @@optimizer_switch;
SET @@optimizer_switch="derived_merge=off";

# use sql_buffer_result
--disable_result_log
explain format = json select sql_buffer_result * from t1;
--enable_result_log
select sql_buffer_result * from t1;
--disable_result_log
explain format = json select sql_buffer_result * from t2;
--enable_result_log
select sql_buffer_result * from t2;

#
# If order by is not simple order
# If not (sort_order only uses fields from head table and the head table is not left join table)
--disable_result_log
explain format = json select * from t1 left join t2 on t1.b = t2.b order by t1.a;
--enable_result_log
select * from t1 left join t2 on t1.b = t2.b order by t1.a;

#
# If sorting by a sp or udf function due to their expensive and probably non-deterministic nature.
--disable_result_log
explain format = json select * from t1 order by func1(a);
--enable_result_log
select * from t1 order by func1(a);

--source include/turn_off_only_full_group_by.inc

# order by aggregate function
--disable_result_log
explain format = json select a from t1 group by b order by sum(a);
--enable_result_log
select a from t1 group by b order by sum(a);

# order by aggregate -- In order to make it simple, we make group by index
--disable_result_log
explain format = json select a from t7 group by b order by sum(a);
--enable_result_log
select a from t7 group by b order by sum(a);

#
# a select with join and sql_big_result(only can work with group by or distinct)
--disable_result_log
explain format = json select sql_big_result distinct * from t1, t2;
--enable_result_log
--sorted_result
select sql_big_result distinct * from t1, t2;
--disable_result_log
explain format = json select sql_big_result t1.b from t1, t2 group by t1.b;
--enable_result_log
--sorted_result
select sql_big_result t1.b from t1, t2 group by t1.b;
--disable_result_log
explain format = json select sql_big_result distinct t1.a from t1, t2 group by t1.a, t1.b;
--enable_result_log
--sorted_result
select sql_big_result distinct t1.a from t1, t2 group by t1.a, t1.b;

#
# a select with join and sorting
--disable_result_log
explain format = json select t1.* from t1 inner join t2 on t1.a = t2.a order by t2.a;
--enable_result_log
select t1.* from t1 inner join t2 on t1.a = t2.a order by t2.a;
--disable_result_log
explain format = json select t1.* from t1, t2 where t1.a = t2.a order by t1.a;
--enable_result_log
select t1.* from t1, t2 where t1.a = t2.a order by t1.a;

#
# a select with join and grouping that uses tmp table for grouping
--disable_result_log
explain format = json select t1.* from t1 inner join t2 where t1.a = t2.a group by t1.a;
--enable_result_log
select t1.* from t1 inner join t2 where t1.a = t2.a group by t1.a;
--disable_result_log
explain format = json select t1.* from t1 inner join t2 where t1.a = t2.a group by t2.a;
--enable_result_log
select t1.* from t1 inner join t2 where t1.a = t2.a group by t2.a;

#
# there is an order by clause and a different group by clause
--disable_result_log
explain format = json select a from t1 group by a order by a;
--enable_result_log
select a from t1 group by a order by a;
--disable_result_log
explain format = json select b from t1 group by b order by a;
--enable_result_log
select b from t1 group by b order by a;

#
# select distinct that uses tmp table for duplicate elimination
--disable_result_log
explain format = json select distinct t1.a from t1, t2 where  t1.b = t2.b;
--enable_result_log
select distinct t1.a from t1, t2 where  t1.b = t2.b;

#
# remove_dup_with_hash_index
--disable_result_log
explain format = json select distinct a from t1 group by b,a;
--enable_result_log
--sorted_result
select distinct a from t1 group by b,a;

#
# remove_dup_with_compare
set @old_sort_buffer_size = @@sort_buffer_size;
set @@sort_buffer_size = 32804;
--disable_result_log
explain format = json select distinct b from t3 group by a having count(*) > 1;
--enable_result_log
--sorted_result
select distinct b from t3 group by a having count(*) > 1;
--sorted_result
select distinct b from t3 group by a;
set @@sort_buffer_size = @old_sort_buffer_size;

#
# special case for distinct
--disable_result_log
explain format = json select distinct t4.a,e from t4, t1 order by t4.b;
--enable_result_log
select distinct t4.a,e from t4, t1 order by t4.b;

#
# union
--disable_result_log
explain format = json select * from t1 union all select * from t2;
--enable_result_log
select * from t1 union all select * from t2;

# union distinct with global order by filesort
--disable_result_log
explain format = json select * from t1 union select * from t2 order by b;
--enable_result_log
select * from t1 union select * from t2 order by b;

#
# select with a single derived table
--disable_result_log
explain format = json select tt.* from (select * from t1) as tt;
--enable_result_log
select tt.* from (select * from t1) as tt;
# Cost estimates are unstable since t5 pages may have been flushed from buffer
# pool since table was created.  Do a table scan to reload pages.
select * from t5;
# select with a derived table that builds index over the derived table
--disable_result_log
explain format = json select b from (select b from t5 where b > 5) tt;
--enable_result_log
select b from (select b from t5 where b > 5) tt;
# select with mutiple derived tables
--sorted_result
--disable_result_log
explain format = json select * from (select b from t1) as t1, (select b from t2) as t2;
--enable_result_log
--sorted_result
select * from (select b from t1) as t1, (select b from t2) as t2;
# derived table uses 'ref' scan
--disable_result_log
explain format = json select * from t1, (select * from t6) as t where t.a =5;
--enable_result_log
select * from t1, (select * from t6) as t where t.a =5;

#
# materialized semi-join
--disable_result_log
explain format = json select * from t1 where t1.a in (select a from t2) ;
--enable_result_log
select * from t1 where t1.a in (select a from t2) ;
# tmp table for semi-join weedout
if (`select locate('materialization', @@optimizer_switch) > 0`)
{
	set optimizer_switch='materialization=off';
}
if (`select locate('firstmatch', @@optimizer_switch) > 0`)
{
	set optimizer_switch='firstmatch=off';
}
if (`select locate('loosescan', @@optimizer_switch) > 0`)
{
	set optimizer_switch='loosescan=off';
}
--disable_result_log
explain format = json select * from t1 where t1.a in (select a from t2) ;
--enable_result_log
select * from t1 where t1.a in (select a from t2) ;
set optimizer_switch='default,derived_merge=off';

#
# muti-update
if (!$skip_update)
{
	--disable_result_log
	explain format = json update t1, t2 set t1.a = 9, t2.a = 10 where t1.b = 'a';
	--enable_result_log
	update t1, t2 set t1.a = 9, t2.a = 10 where t1.b = 'a';
	select * from t2;
}

#
# count with distinct
--disable_result_log
explain format = json select count(distinct a) from t1;
--enable_result_log
select count(distinct a) from t1;

#
# group_concat
--disable_result_log
explain format = json select b, group_concat(a) from t1;
--enable_result_log
select b, group_concat(a) from t1;

#
# quick group
select * from t1;
--disable_result_log
explain format = json select b, count(a), sum(a), avg(a), std(a),
	min(a), max(a), bit_and(a), bit_or(a) from t1 group by b;
--enable_result_log
select b, count(a), sum(a), avg(a), std(a), min(a),
	max(a), bit_and(a), bit_or(a) from t1 group by b;

#
# choose innodb to create tmp table
--disable_warnings
select sql_big_result * from performance_schema.global_variables where variable_name like 'internal_tmp_mem_storage_engine';
--enable_warnings

#
# union distinct and all
select 1 union distinct select 1 union all select 1;

#
# enable and disable indexes
select alias1.`col_int_key` AS field1 from (t8 as alias1) where (5) in
(select 7 union select 1) order by alias1.`col_varchar_key` asc;

#
# Exercises select from multiple non-temporary tables that result in
# insert .... select kind of semantics on intrinsic temporary table.
select count(table2.col_date_key) as field1 from
	(t10 as table1 inner join
 	((t10 as table2 right join
   	t9 as table3 on
   	((table3.pk = table2.pk) or
    	(table3.pk = table2.col_int_key))))
 		on ((table3.pk < table2.col_int_nokey)))
	order by field1 ASC;

#
# trying to create index on blob column
select distinct table1.col_varchar_1024_utf8_key as field1
	from t11 as table1 left join t11 as table2 on
	table1.col_varchar_1024_utf8 = table2.col_varchar_1024_latin1_key
	order by field1;

#
# nulls_equal use-case
select t from t12 group by c;

#
# complex queries resulting in multiple intrinsic temporary tables
select count(outr.col_varchar_nokey) AS x
from t13 as outr2 left join
	(select * from t13) as outr
	on (outr2.col_time_key > outr.col_date_key)
	where outr.pk in
	(select innr.col_int_nokey AS y from
	 (select * from t14) as innr2 left join
	 (select * from t14) AS innr
	 ON (innr2.col_int_nokey = innr.col_int_key)
	 where innr.col_varchar_nokey > innr.col_varchar_key
	 or not outr.pk > 9)
	or outr.col_varchar_key is null;

--source include/restore_sql_mode_after_turn_off_only_full_group_by.inc

SET @@optimizer_switch=@optimizer_switch_saved;
