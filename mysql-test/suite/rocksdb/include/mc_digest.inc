# Sets $mc_digest to a digest of all
# mc tables as the current connection sees them: row count and checksum of a
# forced scan of every index. Two digests are equal only if every index
# returns the same rows.
--disable_query_log
let $mc_digest = `SELECT CONCAT_WS('|',
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(id))) FROM mc.t_pk
     FORCE INDEX (PRIMARY) WHERE id > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', a, b))))
     FROM mc.t_hpk FORCE INDEX (ka) WHERE a > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', a, b))))
     FROM mc.t_hpk FORCE INDEX (kb) WHERE b >= ''),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))))
     FROM mc.t_multi FORCE INDEX (PRIMARY) WHERE id > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))))
     FROM mc.t_multi FORCE INDEX (ku) WHERE u > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))))
     FROM mc.t_multi FORCE INDEX (kv) WHERE v >= ''),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, u, CAST(v AS BINARY), c))))
     FROM mc.t_multi FORCE INDEX (kc) WHERE c >= ''),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', u, CAST(v AS BINARY)))))
     FROM mc.t_multi FORCE INDEX (kcov) WHERE u > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b))))
     FROM mc.t_cf FORCE INDEX (PRIMARY) WHERE id > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b))))
     FROM mc.t_cf FORCE INDEX (ka) WHERE a > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', id, a, b))))
     FROM mc.t_cf FORCE INDEX (kb) WHERE b >= ''),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', g, id, v))))
     FROM mc.t_part FORCE INDEX (PRIMARY) WHERE g > -2147483648),
  (SELECT CONCAT(COUNT(*), ':', BIT_XOR(CRC32(CONCAT_WS('#', g, id, v))))
     FROM mc.t_part FORCE INDEX (kp) WHERE g > -2147483648))`;
--enable_query_log
