# One round of autoinc_crash_release: start concurrent inserts, SIGKILL the
# server while they run, restart it and check the recovered state.
#
# Parameters:
#   $round        round number, used in the output only
#   $load_script  path of the load generator written by the test

--echo # Round $round
--let $_ai_clients = 4

--remove_files_wildcard $MYSQLTEST_VARDIR/tmp aicr_*
--let $_ai_start = `SELECT COUNT(*) FROM a_int`

--let $_ai_c = 1
while ($_ai_c <= $_ai_clients)
{
  --exec_in_background bash $load_script "$MYSQL" $_ai_c $MYSQLTEST_VARDIR/tmp/aicr_ack_$_ai_c $MYSQLTEST_VARDIR/tmp/aicr_done_$_ai_c
  --inc $_ai_c
}

# Let the load run, then kill the server in the middle of it.
--let $wait_timeout = 300
--let $wait_condition = SELECT COUNT(*) >= $_ai_start + 300 FROM a_int
--source include/wait_condition.inc
--sleep 1
--source include/kill_mysqld.inc

# Wait until every client has noticed that the server is gone.
--perl
  my $dir = "$ENV{MYSQLTEST_VARDIR}/tmp";
  for (1 .. 3000) {
    my $done = grep { -e "$dir/aicr_done_$_" } 1 .. 4;
    exit 0 if $done == 4;
    select(undef, undef, undef, 0.1);
  }
  die "load generators did not stop";
EOF

--source include/start_mysqld.inc

# Load the acknowledged rows into a helper table.
--let CHK_FILE = $MYSQLTEST_VARDIR/tmp/aicr_chk.inc
--perl
  use strict;
  my $tmp = "$ENV{MYSQLTEST_VARDIR}/tmp";
  my @rows;
  for my $c (1 .. 4) {
    my $f = "$tmp/aicr_ack_$c";
    next unless -e $f;
    open(my $fh, '<', $f) or die "open $f: $!";
    while (my $l = <$fh>) {
      chomp $l;
      my ($t, $tag, $id) = split / /, $l;
      next unless defined $id && $tag =~ /^\d+$/;
      $id = ($id =~ /^\d+$/) ? $id : 'NULL';
      push @rows, "('$t', $tag, $id)";
    }
    close $fh;
  }
  open(my $oh, '>', $ENV{CHK_FILE}) or die "open $ENV{CHK_FILE}: $!";
  print $oh "--disable_query_log\n";
  while (my @batch = splice(@rows, 0, 500)) {
    print $oh "INSERT INTO chk_ack VALUES " . join(',', @batch) . ";\n";
  }
  print $oh "--enable_query_log\n";
  close $oh;
EOF

CREATE TABLE chk_ack (t VARCHAR(10), tag BIGINT, id BIGINT, KEY (t, tag))
  ENGINE=InnoDB;
--source $CHK_FILE
--remove_file $CHK_FILE

SELECT COUNT(*) >= 100 AS load_ran FROM chk_ack;

# Every acknowledged row survived, with the id the client was given.
SELECT COUNT(*) AS acked_lost FROM chk_ack k
  WHERE NOT EXISTS (SELECT 1 FROM a_int  r WHERE k.t = 'a_int'  AND r.tag = k.tag)
    AND NOT EXISTS (SELECT 1 FROM a_part r WHERE k.t = 'a_part' AND r.tag = k.tag)
    AND NOT EXISTS (SELECT 1 FROM a_sk   r WHERE k.t = 'a_sk'   AND r.tag = k.tag)
    AND NOT EXISTS (SELECT 1 FROM a_ofs  r WHERE k.t = 'a_ofs'  AND r.tag = k.tag);
SELECT COUNT(*) AS acked_id_changed FROM chk_ack k
  WHERE k.id IS NOT NULL AND NOT (
       (k.t = 'a_int'  AND EXISTS (SELECT 1 FROM a_int  r WHERE r.tag = k.tag AND r.id = k.id))
    OR (k.t = 'a_part' AND EXISTS (SELECT 1 FROM a_part r WHERE r.tag = k.tag AND r.id = k.id))
    OR (k.t = 'a_sk'   AND EXISTS (SELECT 1 FROM a_sk   r WHERE r.tag = k.tag AND r.id = k.id))
    OR (k.t = 'a_ofs'  AND EXISTS (SELECT 1 FROM a_ofs  r WHERE r.tag = k.tag AND r.id = k.id)));

# Rolled-back inserts left no rows.
SELECT COUNT(*) AS rolled_back_rows FROM a_part WHERE tag <= -1000000;

# No id appears twice. a_sk has no unique key on id, so only the counter
# prevents duplicates there.
SELECT COUNT(*) - COUNT(DISTINCT id) AS a_sk_duplicate_ids FROM a_sk;
SELECT COUNT(*) AS a_ofs_ids_off_pattern FROM a_ofs WHERE id % 3 <> 2;

# The counter the server reports is above every stored id.
SELECT COUNT(*) AS counters_not_above_max FROM (
  SELECT 'a_int' AS t, MAX(id) AS m FROM a_int
  UNION ALL SELECT 'a_part', MAX(id) FROM a_part
  UNION ALL SELECT 'a_sk', MAX(id) FROM a_sk
  UNION ALL SELECT 'a_ofs', MAX(id) FROM a_ofs) mx
  JOIN information_schema.TABLES it
    ON it.table_schema = 'test' AND it.table_name = mx.t
  WHERE it.AUTO_INCREMENT <= mx.m;

# New rows get ids above every stored id. Marker tags differ per round.
--let $_ai_t1 = `SELECT -($round * 100 + 1)`
--let $_ai_t2 = `SELECT -($round * 100 + 2)`
--let $_ai_t3 = `SELECT -($round * 100 + 3)`
--let $_ai_m_int = `SELECT MAX(id) FROM a_int`
--let $_ai_m_part = `SELECT MAX(id) FROM a_part`
--let $_ai_m_sk = `SELECT MAX(id) FROM a_sk`
--let $_ai_m_ofs = `SELECT MAX(id) FROM a_ofs`
--eval INSERT INTO a_int (tag, c) VALUES ($_ai_t1, 0), ($_ai_t2, 0), ($_ai_t3, 0)
--eval INSERT INTO a_part (tag) VALUES ($_ai_t1), ($_ai_t2), ($_ai_t3)
--eval INSERT INTO a_sk (pk, tag) VALUES ($_ai_t1, $_ai_t1), ($_ai_t2, $_ai_t2), ($_ai_t3, $_ai_t3)
SET SESSION auto_increment_increment = 3, auto_increment_offset = 2;
--eval INSERT INTO a_ofs (tag) VALUES ($_ai_t1), ($_ai_t2), ($_ai_t3)
SET SESSION auto_increment_increment = DEFAULT, auto_increment_offset = DEFAULT;
--disable_query_log
--eval SELECT MIN(id) > $_ai_m_int AS a_int_new_ids_above_max FROM a_int WHERE tag IN ($_ai_t1, $_ai_t2, $_ai_t3)
--eval SELECT MIN(id) > $_ai_m_part AS a_part_new_ids_above_max FROM a_part WHERE tag IN ($_ai_t1, $_ai_t2, $_ai_t3)
--eval SELECT MIN(id) > $_ai_m_sk AS a_sk_new_ids_above_max FROM a_sk WHERE tag IN ($_ai_t1, $_ai_t2, $_ai_t3)
--eval SELECT MIN(id) > $_ai_m_ofs AND MAX(id % 3) = 2 AND MIN(id % 3) = 2 AS a_ofs_new_ids_above_max_and_on_pattern FROM a_ofs WHERE tag IN ($_ai_t1, $_ai_t2, $_ai_t3)
--enable_query_log
SELECT COUNT(*) - COUNT(DISTINCT id) AS a_sk_duplicate_ids_after_inserts FROM a_sk;

CHECK TABLE a_int, a_part, a_sk, a_ofs;

DROP TABLE chk_ack;
--remove_files_wildcard $MYSQLTEST_VARDIR/tmp aicr_*
