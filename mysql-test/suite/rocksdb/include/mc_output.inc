# Shows how much output a held manual compaction has written, from the SST
# files that appeared in the RocksDB directory since the mark.
#
# $mc_output_mode  mark: record the highest SST file number (source it before
#                  the compaction starts)
#                  check: count the SST files created since the mark whose
#                  size is at least 8192 bytes, so a finished output file of
#                  the 16k target size of cf_mc, not the small files of other
#                  column families; then compare with $mc_point: none at
#                  'start' (nothing is written before the first key), at
#                  least one at 'middle' and 'completed'. Echoes the result
#                  and, if it is not as expected, a line starting with
#                  '# NO OUTPUT' or '# UNEXPECTED OUTPUT'.
# $mc_output_dir   optional, default $MYSQLTEST_VARDIR/mysqld.1/data/.rocksdb
#
# The rows of the tables are about 1 KB, so that the first 1000 keys the
# filter sees (rocksdb_pause_manual_compaction_middle) are well over the
# 16k target file size of cf_mc and the output files are cut by then.

--let _MCO_MODE = $mc_output_mode
--let _MCO_POINT = $mc_point
--let _MCO_DIR = $mc_output_dir
if (!$mc_output_dir) {
  --let _MCO_DIR = $MYSQLTEST_VARDIR/mysqld.1/data/.rocksdb
}
--let _MCO_MARK = $MYSQLTEST_VARDIR/tmp/mc_output.mark
--let _MCO_RESULT = $MYSQLTEST_VARDIR/tmp/mc_output.result

--perl
  use strict;
  my $dir = $ENV{_MCO_DIR};
  my @sst = glob("$dir/*.sst");
  die "mc_output.inc: no SST files in $dir" unless @sst;
  my %size;
  my $max = 0;
  foreach my $f (@sst) {
    $f =~ m{/0*(\d+)\.sst$} or next;
    $size{$1} = -s $f;
    $max = $1 if $1 > $max;
  }
  if ($ENV{_MCO_MODE} eq 'mark') {
    open(my $m, '>', $ENV{_MCO_MARK}) or die $!;
    print $m "$max\n";
    close($m);
    exit 0;
  }
  open(my $m, '<', $ENV{_MCO_MARK}) or die "mc_output.inc: no mark: $!";
  my $mark = <$m>;
  chomp($mark);
  close($m);
  my $n = grep { $_ > $mark && $size{$_} >= 8192 } keys %size;
  my $point = $ENV{_MCO_POINT};
  my $msg;
  if ($point eq 'start') {
    $msg = $n == 0 ? "# Output when held at start: none"
                   : "# UNEXPECTED OUTPUT at start: $n files";
  } else {
    $msg = $n > 0 ? "# Output when held at $point: written"
                  : "# NO OUTPUT at $point";
  }
  open(my $r, '>', $ENV{_MCO_RESULT}) or die $!;
  print $r "$msg\n";
  close($r);
EOF
if ($mc_output_mode == check) {
  --cat_file $_MCO_RESULT
  --remove_file $_MCO_RESULT
  --remove_file $_MCO_MARK
}
