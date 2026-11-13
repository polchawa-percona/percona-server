# Overwrite bytes inside one block of a RocksDB SST file.
#
# Release-capable: no debug points. The block layout is read from the file
# itself with sst_dump, so no offset is hardcoded.
#
# The server must be shut down before sourcing this file, so that no block of
# the file is cached and the damage is seen on the next read.
#
# Parameters:
#   $corrupt_sst    full path of the SST file
#   $corrupt_where  data_first | data_last | index | filter | footer
#
# Block layout of a block-based table (format_version 6, the RocksDB 9.x
# default): data blocks, then the filter block (if any), then the index
# block, then meta blocks, and the footer at the end of the file. Each block
# is followed by a 5-byte trailer (compression type + checksum).
# sst_dump --command=raw prints every data block handle and the filter and
# index block sizes, which is enough to locate them.

# Pass the parameters through a file: environment variables set with --let
# in a sourced file did not reach the --perl block, and --write_file does
# not expand variables.
--let $corrupt_args = $MYSQLTEST_VARDIR/tmp/corrupt_sst_block.args
--exec echo "$corrupt_sst" > $corrupt_args
--exec echo "$corrupt_where" >> $corrupt_args

--perl
use strict;
use warnings;
use File::Copy;

my $args = "$ENV{'MYSQLTEST_VARDIR'}/tmp/corrupt_sst_block.args";
open(my $afh, '<', $args) or die "open $args: $!";
chomp(my $sst = <$afh>);
chomp(my $where = <$afh>);
close($afh);
unlink($args);
die "no SST file given" unless $sst;
die "no corruption location given" unless $where;
my $dump_tool = $ENV{'MYSQL_SST_DUMP'} or die "MYSQL_SST_DUMP is not set";

# sst_dump writes <name>_dump.txt (the .sst suffix replaced) next to the file
# it reads, so work on a copy outside the RocksDB data directory.
my $tmp = "$ENV{'MYSQLTEST_VARDIR'}/tmp/corrupt_sst_layout.sst";
copy($sst, $tmp) or die "copy $sst: $!";
system("$dump_tool --file=$tmp --command=raw > /dev/null 2>&1") == 0
  or die "sst_dump failed";
(my $dump = $tmp) =~ s/\.sst$/_dump.txt/;
open(my $fh, '<', $dump) or die "open $dump: $!";

my @data;            # [offset, size] of each data block, in file order
my ($filter_size, $index_size) = (0, 0);
while (my $line = <$fh>) {
  if ($line =~ /:\s*[0-9A-F]*\s+offset\s+(\d+)\s+size\s+(\d+)\s*$/) {
    push @data, [$1, $2];
  } elsif ($line =~ /^\s*filter block size:\s*(\d+)/) {
    $filter_size = $1;
  } elsif ($line =~ /^\s*index block size[^:]*:\s*(\d+)/) {
    $index_size = $1;
  }
}
close($fh);
unlink($dump, $tmp);
die "no data block handles found in sst_dump output" unless @data;

my $trailer  = 5;
my $data_end = $data[-1][0] + $data[-1][1] + $trailer;
my $file_size = -s $sst;

my ($off, $len);
if ($where eq 'data_first') {
  ($off, $len) = ($data[0][0] + $data[0][1] / 2, 16);
} elsif ($where eq 'data_last') {
  ($off, $len) = ($data[-1][0] + $data[-1][1] / 2, 16);
} elsif ($where eq 'filter') {
  die "the SST file has no filter block" unless $filter_size > 0;
  ($off, $len) = ($data_end + $filter_size / 2, 16);
} elsif ($where eq 'index') {
  die "index block size is unknown" unless $index_size > 0;
  my $index_off = $data_end;
  $index_off += $filter_size + $trailer if $filter_size > 0;
  ($off, $len) = ($index_off + $index_size / 2, 8);
} elsif ($where eq 'footer') {
  # The last 8 bytes of the file are the table magic number.
  ($off, $len) = ($file_size - 8, 8);
} else {
  die "unknown corruption location '$where'";
}
$off = int($off);
die "offset $off outside the file" if $off + $len > $file_size;

open(my $out, '+<:raw', $sst) or die "open $sst: $!";
seek($out, $off, 0) or die "seek: $!";
my $orig;
read($out, $orig, $len) == $len or die "read: $!";
my $bad = join('', map { chr(ord($_) ^ 0xFF) } split(//, $orig));
seek($out, $off, 0) or die "seek: $!";
print $out $bad;
close($out) or die "close: $!";
EOF
