#
# The statement of crashpoint_bulk_load: a bulk load of 3000 sorted rows,
# which is ingested when the bulk load mode is switched off.
#
SET SESSION cte_max_recursion_depth = 10000;
SET SESSION rocksdb_bulk_load_size = 1000;
SET SESSION rocksdb_bulk_load = 1;
INSERT INTO cp.t SELECT seq, seq % 97, CONCAT('r', seq) FROM (WITH RECURSIVE s(seq) AS (SELECT 1 UNION ALL SELECT seq + 1 FROM s WHERE seq < 3000) SELECT seq FROM s) AS x;
SET SESSION rocksdb_bulk_load = 0;
