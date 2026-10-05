# Verify that a MyRocks secondary index returns the same data as the
# primary key, for tests ported from suite/innodb.
#
# Parameters:
#   $cpi_table  table name
#   $cpi_index  secondary index name
#   $cpi_pk     primary key column list (comma separated)
#   $cpi_cols   indexed column list, as used in the index (comma separated)
#   $cpi_join   optional join condition between aliases x and y on all
#               indexed columns; defaults to y.$cpi_cols = x.$cpi_cols, which
#               only works for a single column
#
# Two checks are made, and each one prints a single line:
#   1. A full scan through the index, which is index-only (covering) when the
#      columns are fully stored and unpackable, returns the same
#      (pk, HEX(cols)) set as a scan that ignores the index.
#   2. Equality lookups through the index, with every stored value as the
#      key, find the same number of rows as lookups that ignore the index.
#
# The hash is a SUM of CRC32 values: order independent, and duplicate rows
# add up instead of cancelling out.

--let $_cpi_hex = CONCAT_WS(':', $cpi_pk, HEX(CONCAT_WS('|', $cpi_cols)))

--let $_cpi_via_index = `SELECT CONCAT(COUNT(*), '/', SUM(CRC32($_cpi_hex))) FROM $cpi_table FORCE INDEX($cpi_index)`
--let $_cpi_via_pk = `SELECT CONCAT(COUNT(*), '/', SUM(CRC32($_cpi_hex))) FROM $cpi_table IGNORE INDEX($cpi_index)`
if ($_cpi_via_index == $_cpi_via_pk)
{
  --echo # $cpi_table.$cpi_index: index scan matches primary key scan
}
if ($_cpi_via_index != $_cpi_via_pk)
{
  --echo # $cpi_table.$cpi_index: MISMATCH index scan $_cpi_via_index, primary key scan $_cpi_via_pk
  --eval SELECT $cpi_pk, HEX(CONCAT_WS('|', $cpi_cols)) FROM $cpi_table FORCE INDEX($cpi_index) ORDER BY $cpi_pk
  --eval SELECT $cpi_pk, HEX(CONCAT_WS('|', $cpi_cols)) FROM $cpi_table IGNORE INDEX($cpi_index) ORDER BY $cpi_pk
}

# Equality lookups. NULLs never match, on both sides.
--let $_cpi_on = $cpi_join
if (!$_cpi_on)
{
  --let $_cpi_on = y.$cpi_cols = x.$cpi_cols
}
--let $_cpi_lookup_index = `SELECT COUNT(*) FROM $cpi_table AS x JOIN $cpi_table AS y FORCE INDEX($cpi_index) ON $_cpi_on`
--let $_cpi_lookup_pk = `SELECT COUNT(*) FROM $cpi_table AS x JOIN $cpi_table AS y IGNORE INDEX($cpi_index) ON $_cpi_on`
if ($_cpi_lookup_index == $_cpi_lookup_pk)
{
  --echo # $cpi_table.$cpi_index: equality lookups match ($_cpi_lookup_index pairs)
}
if ($_cpi_lookup_index != $_cpi_lookup_pk)
{
  --echo # $cpi_table.$cpi_index: MISMATCH equality lookups via index $_cpi_lookup_index, without index $_cpi_lookup_pk
}

--let $cpi_join =
