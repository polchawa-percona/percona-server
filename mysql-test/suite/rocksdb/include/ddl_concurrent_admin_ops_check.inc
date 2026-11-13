#
# Checks one table of rocksdb.ddl_concurrent_admin_ops*: the row count and
# checksum through the primary key and through the secondary index must match
# the expected values, and CHECK TABLE must pass.
#
# Parameters:
#   $chk_table   table name
#   $chk_index   name of a secondary index that covers column b, or empty
#   $chk_rows    expected row count
#   $chk_sum     expected SUM(a) + SUM(b)
#

--let $_exp = $chk_rows:$chk_sum
--let $_pk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(a) + SUM(b), 0)) FROM $chk_table FORCE INDEX (PRIMARY)`
if ($_pk != $_exp)
{
  --echo Mismatch through PRIMARY of $chk_table: $_pk, expected $chk_rows:$chk_sum
  --die Primary key of $chk_table lost or gained rows
}
if ($chk_index)
{
  --let $_sk = `SELECT CONCAT(COUNT(*), ':', COALESCE(SUM(a) + SUM(b), 0)) FROM $chk_table FORCE INDEX ($chk_index) WHERE b >= 0`
  if ($_sk != $_exp)
  {
    --echo Mismatch through $chk_index of $chk_table: $_sk, expected $chk_rows:$chk_sum
    --die Secondary index of $chk_table does not match its primary key
  }
}
--let $_ct = query_get_value(CHECK TABLE $chk_table, Msg_text, 1)
if ($_ct != OK)
{
  --echo CHECK TABLE $chk_table: $_ct
  --die CHECK TABLE failed
}
