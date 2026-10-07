# Helper of errinj_xa_run.inc: runs $errinj_xa_q unless the server is gone,
# and records the first error in $errinj_err. After an error the remaining
# statements of the transaction are skipped ($errinj_xa_stop).
--let $errinj_xa_e = 0
if (!$errinj_xa_stop) {
  --disable_abort_on_error
  eval $errinj_xa_q;
  --let $errinj_xa_e = $mysql_errno
  --enable_abort_on_error
  if ($errinj_xa_e == 2006) {
    --let $errinj_xa_e = 2013
  }
  if ($errinj_xa_e) {
    if (!$errinj_err) {
      --let $errinj_err = $errinj_xa_e
    }
  }
  if ($errinj_xa_e) {
    --let $errinj_xa_stop = 1
  }
  if ($errinj_xa_e == 2013) {
    --let $errinj_xa_dead = 1
  }
}
