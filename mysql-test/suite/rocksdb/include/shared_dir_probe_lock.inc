# Report whether another process can take an exclusive fcntl lock on
# $WAL_LOCK_FILE, the lock MyRocks holds on a directory it must not share.
# Linux only: struct flock is packed by hand.
perl;
  use Fcntl qw(F_SETLK F_WRLCK SEEK_SET O_RDWR);
  my $file = $ENV{WAL_LOCK_FILE};
  sysopen(my $fh, $file, O_RDWR) or die "Can't open $file: $!";
  my $flock = pack('s s x4 q q i x4', F_WRLCK, SEEK_SET, 0, 0, 0);
  if (fcntl($fh, F_SETLK, $flock)) {
    print "Lock file is free.\n";
  } else {
    print "Lock file is locked by a running instance.\n";
  }
  close($fh);
EOF
