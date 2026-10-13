# Compares index $cp_idx of the MyRocks table $cp_t with its InnoDB mirror
# <table>_ref, partition by partition: row count and the checksum of
# $cp_expr, an expression over columns the index covers. An empty $cp_idx
# scans the table without forcing an index.
--disable_query_log
let $cp_ref = `SELECT CONCAT('$cp_t', '_ref')`;
let $cp_force = ;
if ($cp_idx)
{
  let $cp_force = FORCE INDEX ($cp_idx);
}
let $cp_parts = `SELECT GROUP_CONCAT(PARTITION_NAME ORDER BY PARTITION_ORDINAL_POSITION) FROM INFORMATION_SCHEMA.PARTITIONS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$cp_t'`;
let $cp_ref_parts = `SELECT GROUP_CONCAT(PARTITION_NAME ORDER BY PARTITION_ORDINAL_POSITION) FROM INFORMATION_SCHEMA.PARTITIONS WHERE TABLE_SCHEMA = 'test' AND TABLE_NAME = '$cp_ref'`;
if ($cp_parts != $cp_ref_parts)
{
  --echo $cp_t: partitions $cp_parts, mirror has $cp_ref_parts
}
let $cp_bad = 0;
let $cp_n = 0;
let $cp_list = $cp_parts;
while ($cp_list)
{
  let $cp_p = `SELECT SUBSTRING_INDEX('$cp_list', ',', 1)`;
  let $cp_list = `SELECT SUBSTRING('$cp_list', LENGTH('$cp_p') + 2)`;
  let $cp_got = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(CRC32($cp_expr)), 0)) FROM $cp_t PARTITION ($cp_p) $cp_force`;
  let $cp_exp = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(CRC32($cp_expr)), 0)) FROM $cp_ref PARTITION ($cp_p)`;
  if ($cp_got != $cp_exp)
  {
    --echo $cp_t.$cp_p index '$cp_idx': count:checksum $cp_got, expected $cp_exp
    inc $cp_bad;
  }
  inc $cp_n;
}
if (!$cp_bad)
{
  --echo $cp_t index '$cp_idx': $cp_n partitions match the mirror
}
--enable_query_log
