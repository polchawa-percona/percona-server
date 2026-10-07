# Helper of the XA error injection tests, run after a restart: finishes a
# recovered prepared transaction 'x' as the workload would have, with XA
# $errinj_xa_recover (COMMIT or ROLLBACK).
--let $errinj_xa_left = query_get_value(XA RECOVER, data, 1)
if ($errinj_xa_left == x) {
  eval XA $errinj_xa_recover 'x';
}
