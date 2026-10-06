# Helper of errinj_loop.inc: runs $errinj_state_proc if it is set.
if ($errinj_state_proc) {
  eval $errinj_state_proc;
}
