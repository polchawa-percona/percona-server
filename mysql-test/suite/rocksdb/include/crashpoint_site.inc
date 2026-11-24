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
# If $cp_log_marker is set (crashpoint_loop.inc appends a unique marker
# line to the error log when it starts), only the part of the log after
# the last occurrence of the marker is counted, so the counts belong to
# the current loop alone and cannot be shifted by the lines of earlier
# tests of the same MTR worker. The log is read ONCE per call (into a
# single query), so the counts and the site come from the same
# snapshot of the growing log.
#
--let $_cp_lf = $MYSQLTEST_VARDIR/log/mysqld.1.err
--let $_cp_src = LOAD_FILE('$_cp_lf')
if ($cp_log_marker) {
  # NULL if the marker is missing, so that the read check below fails.
  --let $_cp_src = IF(LOCATE('$cp_log_marker', LOAD_FILE('$_cp_lf')) > 0, SUBSTRING_INDEX(LOAD_FILE('$_cp_lf'), '$cp_log_marker', -1), NULL)
}
# One query, one snapshot: "<fired> <dropped> x<hex(site)> x<hex(site_full)>"
# or "NULL" (no log, or no marker in it). The marker test and the substring read the file twice inside
# one statement; LOAD_FILE of a growing log can differ between them only
# by lines appended after the marker, which do not move it.
--let $_cp_all = `SELECT IF(s IS NULL, 'NULL', CONCAT_WS(' ', ROUND((LENGTH(s) - LENGTH(REPLACE(s, 'crash-point hook fired at ', ''))) / LENGTH('crash-point hook fired at ')), ROUND((LENGTH(s) - LENGTH(REPLACE(s, 'test: unsynced data dropped', ''))) / LENGTH('test: unsynced data dropped')), IF(LOCATE('crash-point hook fired at ', s) > 0, CONCAT('x', HEX(SUBSTRING_INDEX(SUBSTRING_INDEX(s, 'crash-point hook fired at ', -1), ' ', 1))), 'x'), IF(LOCATE('crash-point hook fired at ', s) > 0, CONCAT('x', HEX(SUBSTRING_INDEX(SUBSTRING_INDEX(s, 'crash-point hook fired at ', -1), ' (nth durable', 1))), 'x'))) FROM (SELECT $_cp_src AS s) cp_log_snapshot`
if ($_cp_all == NULL)
{
  --die crashpoint_site.inc: cannot read the error log (or the marker $cp_log_marker is not in it)
}
--let $cp_fired = `SELECT SUBSTRING_INDEX('$_cp_all', ' ', 1)`
--let $cp_dropped = `SELECT SUBSTRING_INDEX(SUBSTRING_INDEX('$_cp_all', ' ', 2), ' ', -1)`
--let $cp_site = none
--let $cp_site_full = none
if ($cp_fired)
{
  --let $cp_site = `SELECT UNHEX(SUBSTRING(SUBSTRING_INDEX(SUBSTRING_INDEX('$_cp_all', ' ', 3), ' ', -1), 2))`
  # "<site>" or "<site> [<detail>]": the text between the site and " (nth durable".
  --let $cp_site_full = `SELECT UNHEX(SUBSTRING(SUBSTRING_INDEX('$_cp_all', ' ', -1), 2))`
}
--let $_cp_lf =
--let $_cp_src =
--let $_cp_all =
