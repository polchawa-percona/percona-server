# Consistency check for the mc tables: every secondary key must return the
# same rows as the primary key (or the hidden primary key), checked with a
# forced range scan of each index. Prints the row count and a checksum per
# table, so a result file also catches lost or resurrected rows. Then runs
# CHECK TABLE.
# $mc_check_label: printed in the header.
--echo # check: $mc_check_label
--disable_query_log
SELECT 't_pk' AS tbl, COUNT(*) AS n, BIT_XOR(CRC32(id)) AS sum
  FROM mc.t_pk FORCE INDEX (PRIMARY) WHERE id > -2147483648;

SELECT 't_hpk' AS tbl, x.n, x.s AS sum,
       x.n = y.n AND x.s = y.s AS ka_ok, x.n = z.n AND x.s = z.s AS kb_ok
  FROM (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', a, b))) s
          FROM mc.t_hpk IGNORE INDEX (ka, kb)) x,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', a, b))) s
          FROM mc.t_hpk FORCE INDEX (ka) WHERE a > -2147483648) y,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', a, b))) s
          FROM mc.t_hpk FORCE INDEX (kb) WHERE b >= '') z;

SELECT 't_multi' AS tbl, p.n, p.s AS sum,
       p.n = ku.n AND p.s = ku.s AS ku_ok, p.n = kv.n AND p.s = kv.s AS kv_ok,
       p.n = kc.n AND p.s = kc.s AS kc_ok,
       p.n = kcov.n AND p.cs = kcov.cs AS kcov_ok
  FROM (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))) s,
               BIT_XOR(CRC32(CONCAT_WS('#', u, CAST(v AS BINARY)))) cs
          FROM mc.t_multi FORCE INDEX (PRIMARY) WHERE id > -2147483648) p,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))) s
          FROM mc.t_multi FORCE INDEX (ku) WHERE u > -2147483648) ku,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))) s
          FROM mc.t_multi FORCE INDEX (kv) WHERE v >= '') kv,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))) s
          FROM mc.t_multi FORCE INDEX (kc) WHERE c >= '') kc,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', u, CAST(v AS BINARY)))) cs
          FROM mc.t_multi FORCE INDEX (kcov) WHERE u > -2147483648) kcov;

SELECT 't_cf' AS tbl, p.n, p.s AS sum,
       p.n = ka.n AND p.s = ka.s AS ka_ok, p.n = kb.n AND p.s = kb.s AS kb_ok
  FROM (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', id, a, b))) s
          FROM mc.t_cf FORCE INDEX (PRIMARY) WHERE id > -2147483648) p,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', id, a, b))) s
          FROM mc.t_cf FORCE INDEX (ka) WHERE a > -2147483648) ka,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', id, a, b))) s
          FROM mc.t_cf FORCE INDEX (kb) WHERE b >= '') kb;

SELECT 't_part' AS tbl, p.n, p.s AS sum, p.n = k.n AND p.s = k.s AS kp_ok
  FROM (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', g, id, v))) s
          FROM mc.t_part FORCE INDEX (PRIMARY) WHERE g > -2147483648) p,
       (SELECT COUNT(*) n, BIT_XOR(CRC32(CONCAT_WS('#', g, id, v))) s
          FROM mc.t_part FORCE INDEX (kp) WHERE g > -2147483648) k;

CHECK TABLE mc.t_pk, mc.t_hpk, mc.t_multi, mc.t_cf, mc.t_part;
--enable_query_log
