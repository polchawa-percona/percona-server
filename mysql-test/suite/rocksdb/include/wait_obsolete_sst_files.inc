# Waits up to 30 s until no obsolete SST file is left on disk
# ($sst_on_disk == $sst_live), then leaves both variables set.
--let $_wosf_n = 300
while ($_wosf_n)
{
  --source suite/rocksdb/include/sst_files_on_disk.inc
  if ($sst_on_disk == $sst_live)
  {
    --let $_wosf_n = 0
  }
  if ($sst_on_disk != $sst_live)
  {
    --dec $_wosf_n
    --sleep 0.1
  }
}
