# Checks the bulk load temporary SST files in a RocksDB data directory while
# the server is down (include/power_loss_now_files.inc).
#
# usage: power_loss_tmp_files.pl <rocksdb dir> <power loss: 0|1>
#
# A bulk load in progress leaves *.bulk_load.tmp.sst files that the server
# removes at the next startup, so they can only be seen while it is down.
# They are created and never synced to a directory, so with the power loss
# simulation (which removes the files whose directory entries were never
# synced) none may be left, and after a plain kill they must be there.
# Prints the same line in both cases when that is what was found, FAIL
# otherwise, so that one result file covers both combinations.
use strict;
my ($dir, $power) = @ARGV;
opendir(my $d, $dir) or die "cannot open $dir: $!";
my @tmp = grep { /\.bulk_load\.tmp\.sst$/ } readdir($d);
closedir($d);
if ($power && @tmp == 0) {
  print "ok: the temporary bulk load files are there exactly after a plain kill\n";
} elsif (!$power && @tmp > 0) {
  print "ok: the temporary bulk load files are there exactly after a plain kill\n";
} else {
  print "FAIL: power=$power, temporary bulk load files: " . scalar(@tmp) . "\n";
}
