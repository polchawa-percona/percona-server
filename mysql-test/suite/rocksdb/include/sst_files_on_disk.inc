# Sets $sst_on_disk to the number of SST files in the MyRocks data
# directory and $sst_live to the number of live SST files RocksDB reports.
# Obsolete files that are kept (file deletions disabled) are the difference.
--perl
  my $dir = "$ENV{MYSQLTEST_VARDIR}/mysqld.1/data/.rocksdb";
  opendir(my $dh, $dir) or die "cannot open $dir: $!";
  my $n = grep { /\.sst$/ } readdir($dh);
  closedir($dh);
  open(my $fh, '>', "$ENV{MYSQLTEST_VARDIR}/tmp/sst_files_on_disk.inc") or die;
  print $fh "--let \$sst_on_disk = $n\n";
  close($fh);
EOF
--source $MYSQLTEST_VARDIR/tmp/sst_files_on_disk.inc
--remove_file $MYSQLTEST_VARDIR/tmp/sst_files_on_disk.inc
--let $sst_live = `SELECT COUNT(*) FROM information_schema.ROCKSDB_SST_PROPS`
