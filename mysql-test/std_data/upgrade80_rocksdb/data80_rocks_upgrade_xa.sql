-- Run on Percona Server 8.0 with MyRocks loaded and the binary log
-- enabled. Leaves prepared XA transactions behind for
-- data80_rocks_upgrade_xa.zip; gen_upgrade_datadirs.sh kills the server
-- with SIGKILL afterwards.

CREATE DATABASE upgx;
USE upgx;
CREATE TABLE t_r (id INT PRIMARY KEY, a INT, KEY ka (a)) ENGINE=ROCKSDB;
CREATE TABLE t_i (id INT PRIMARY KEY, a INT) ENGINE=InnoDB;
INSERT INTO t_r VALUES (1, 1), (2, 2), (3, 3);
INSERT INTO t_i VALUES (1, 1);

-- SESSION
USE upgx;
-- Committed XA transaction.
XA START 'upg_committed';
INSERT INTO t_r VALUES (10, 10);
XA END 'upg_committed';
XA PREPARE 'upg_committed';
XA COMMIT 'upg_committed';

-- SESSION
USE upgx;
-- Prepared, MyRocks only: committed after the upgrade.
XA START 'upg_rocks_commit';
INSERT INTO t_r VALUES (20, 20);
UPDATE t_r SET a = 200 WHERE id = 2;
XA END 'upg_rocks_commit';
XA PREPARE 'upg_rocks_commit';

-- SESSION
USE upgx;
-- Prepared, MyRocks only: rolled back after the upgrade.
XA START 'upg_rocks_rollback';
INSERT INTO t_r VALUES (30, 30);
DELETE FROM t_r WHERE id = 3;
XA END 'upg_rocks_rollback';
XA PREPARE 'upg_rocks_rollback';

-- SESSION
USE upgx;
-- Prepared, MyRocks and InnoDB: committed after the upgrade.
XA START 'upg_mixed';
INSERT INTO t_r VALUES (40, 40);
INSERT INTO t_i VALUES (40, 40);
XA END 'upg_mixed';
XA PREPARE 'upg_mixed';
