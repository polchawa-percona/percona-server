# Helper of errinj_loop.inc: arms the error injection in the current
# connection for the iteration $errinj_n.
eval SET SESSION rocksdb_debug_inject_error_class = '$errinj_class';
eval SET SESSION rocksdb_debug_inject_error_status = '$errinj_status';
eval SET SESSION rocksdb_debug_inject_error_nth = $errinj_n;
