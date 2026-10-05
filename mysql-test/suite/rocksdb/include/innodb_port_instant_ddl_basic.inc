##########################################################################
# MyRocks port of suite/innodb/include/instant_ddl_basic.inc (first part):
# ALTER TABLE ADD/DROP COLUMN ... ALGORITHM=INSTANT at different positions for
# a table with
#  - No index defined
#  - Cluster index defined
#  - Secondary index defined
#
# $clust_index : indicates Primary key clause (if any)
# $sec_index   : indicates secondary key clause (if any)
#
# Every ALGORITHM=INSTANT statement goes through
# innodb_port_instant_alter.inc, which records whether MyRocks accepted it and
# otherwise runs it with the default algorithm. The InnoDB metadata dumps
# (print_instant_metadata.inc) are not ported; see
# innodb_port_instant_ddl_misc for the MyRocks DD metadata.
##########################################################################

--echo # ------------------------------------------------------------
--echo # Create a table with 3 columns. [c1, c2, c3]
--echo # ------------------------------------------------------------
--eval CREATE TABLE t1 (c1 char(20), c2 char(20), c3 char(20) $clust_index $sec_index) ENGINE=ROCKSDB

INSERT INTO t1 values ("r1c1", "r1c2", "r1c3");
SELECT * FROM t1 ORDER BY c1;

--echo
--echo # -----------------------ADD COLUMN TEST----------------------
--echo

--echo # ------------------------------------------------------------
--echo # Add a new column at the end. [c1, c2, c3, +c4]
--echo # ------------------------------------------------------------
--let $alter = ALTER TABLE t1 ADD COLUMN c4 char(20) DEFAULT "c4_def"
--source suite/rocksdb/include/innodb_port_instant_alter.inc

INSERT INTO t1 values ("r2c1", "r2c2", "r2c3", "r2c4");
SELECT * FROM t1 ORDER BY c1;

--echo # ------------------------------------------------------------
--echo # Add a new column in between. [c1, +c5, c2, c3, c4]
--echo # ------------------------------------------------------------
--let $alter = ALTER TABLE t1 ADD COLUMN c5 char(20) DEFAULT NULL AFTER c1
--source suite/rocksdb/include/innodb_port_instant_alter.inc

INSERT INTO t1 values ("r3c1", "r3c5", "r3c2", "r3c3", "r3c4");
SELECT * FROM t1 ORDER BY c1;

--echo # ------------------------------------------------------------
--echo # Add a new column at first. [+c6, c1, c5, c2, c3, c4]
--echo # ------------------------------------------------------------
--let $alter = ALTER TABLE t1 ADD COLUMN c6 char(20) DEFAULT NULL FIRST
--source suite/rocksdb/include/innodb_port_instant_alter.inc

INSERT INTO t1 values ("r4c6", "r4c1", "r4c5", "r4c2", "r4c3", "r4c4");
SELECT * FROM t1 ORDER BY c1;


--echo # ------------------------------------------------------------
--echo # Rebuild table. [c6, c1, c5, c2, c3, c4]
--echo # ------------------------------------------------------------
ALTER TABLE t1 force;

SELECT * FROM t1 ORDER BY c1;

--echo
--echo # -----------------------DROP COLUMN TEST---------------------
--echo

--echo # ------------------------------------------------------------
--echo # Drop a column from the end. [c6, c1, c5, c2, c3, -c4]
--echo # ------------------------------------------------------------
--let $alter = ALTER TABLE t1 DROP COLUMN c4
--source suite/rocksdb/include/innodb_port_instant_alter.inc

SELECT * FROM t1 ORDER BY c1;
INSERT INTO t1 values ("r5c6", "r5c1", "r5c5", "r5c2", "r5c3");
SELECT * FROM t1 ORDER BY c1;

--echo # ------------------------------------------------------------
--echo # Drop a column somewhere in between. [c6, c1, -c5, c2, c3] [~c4]
--echo # ------------------------------------------------------------
--let $alter = ALTER TABLE t1 DROP COLUMN c5
--source suite/rocksdb/include/innodb_port_instant_alter.inc

SELECT * FROM t1 ORDER BY c1;
INSERT INTO t1 values ("r6c6", "r6c1", "r6c2", "r6c3");
SELECT * FROM t1 ORDER BY c1;

--echo # ------------------------------------------------------------
--echo # Drop the first column. [-c6, c1, c2, c3] [~c4, ~c5]
--echo # ------------------------------------------------------------
--let $alter = ALTER TABLE t1 DROP COLUMN c6
--source suite/rocksdb/include/innodb_port_instant_alter.inc

SELECT * FROM t1 ORDER BY c1;
INSERT INTO t1 values ("r7c1", "r7c2", "r7c3");
SELECT * FROM t1 ORDER BY c1;

--echo # ------------------------------------------------------------
--echo # ADD and DROP in same statement. [c1, c2, -c3, +c7] [~c4, ~c5, ~c6]
--echo # ------------------------------------------------------------
--let $alter = ALTER TABLE t1 ADD COLUMN c7 char(10) default "c7_def", DROP COLUMN c3
--source suite/rocksdb/include/innodb_port_instant_alter.inc

SELECT * FROM t1 ORDER BY c1;
INSERT INTO t1 values ("r8c1", "r8c2", "r8c7");
SELECT * FROM t1 ORDER BY c1;

--echo # --------------------------------------------------------------------------
--echo # ADD and DROP same column in same statement. [c1, c2, -c7, +c7] [~c4, ~c5, ~c6, ~c3]
--echo # --------------------------------------------------------------------------
--let $alter = ALTER TABLE t1 ADD COLUMN c7 char(10) default "c7_def2", DROP COLUMN c7
--source suite/rocksdb/include/innodb_port_instant_alter.inc

SELECT * FROM t1 ORDER BY c1;
INSERT INTO t1 values ("r9c1", "r9c2", "r9c7");
SELECT * FROM t1 ORDER BY c1;

--echo # ------------------------------------------------------------
--echo # Rebuild table. [c1, c2, c7]
--echo # ------------------------------------------------------------
ALTER TABLE t1 force;

SELECT * FROM t1 ORDER BY c1;
INSERT INTO t1 values ("r10c1", "r10c2", "r10c7");
SELECT * FROM t1 ORDER BY c1;

--echo ###########
--echo # CLEANUP #
--echo ###########
DROP TABLE t1;
