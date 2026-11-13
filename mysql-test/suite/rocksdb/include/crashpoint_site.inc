#
# crashpoint_site.inc
#
# Reads the error log of server 1 (with LOAD_FILE(), the log is in the
# vardir that secure_file_priv allows) and sets
#   $cp_site   the site named by the last "crash-point hook fired at <site>"
#              line ("none" if there is no such line),
#   $cp_site_full the site with its detail as logged, "<site> [<detail>]",
#              (the detail is what the hook says the write is about, e.g.
#              "dict_commit [rename cp.#sql-1_2 -> cp.t]"; no brackets if
#              the site gives none),
#   $cp_fired  the number of such lines in the log so far,
#   $cp_dropped the number of "unsynced data dropped" lines (the power loss
#              simulation writes one per hook crash).
# crashpoint_loop.inc uses the count to prove that every crash it observes
# was made by the hook (exactly one new line per crash), and prints the
# site of every window, so that a change in the sequence of durable
# writes of a statement shows up in the result file.
#
if (!`SELECT LOAD_FILE('$MYSQLTEST_VARDIR/log/mysqld.1.err') IS NOT NULL`)
{
  --die crashpoint_site.inc: cannot read the error log
}
--let $cp_fired = `SELECT ROUND((LENGTH(LOAD_FILE('$MYSQLTEST_VARDIR/log/mysqld.1.err')) - LENGTH(REPLACE(LOAD_FILE('$MYSQLTEST_VARDIR/log/mysqld.1.err'), 'crash-point hook fired at ', ''))) / LENGTH('crash-point hook fired at '))`
--let $cp_dropped = `SELECT ROUND((LENGTH(LOAD_FILE('$MYSQLTEST_VARDIR/log/mysqld.1.err')) - LENGTH(REPLACE(LOAD_FILE('$MYSQLTEST_VARDIR/log/mysqld.1.err'), 'test: unsynced data dropped', ''))) / LENGTH('test: unsynced data dropped'))`
--let $cp_site = none
if ($cp_fired)
{
  --let $cp_site = `SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(LOAD_FILE('$MYSQLTEST_VARDIR/log/mysqld.1.err'), 'crash-point hook fired at ', -1), ' ', 1)`
}
--let $cp_site_full = none
if ($cp_fired)
{
  # "<site>" or "<site> [<detail>]": the text between the site and " (nth durable".
  --let $cp_site_full = `SELECT SUBSTRING_INDEX(SUBSTRING_INDEX(LOAD_FILE('$MYSQLTEST_VARDIR/log/mysqld.1.err'), 'crash-point hook fired at ', -1), ' (nth durable', 1)`
}
