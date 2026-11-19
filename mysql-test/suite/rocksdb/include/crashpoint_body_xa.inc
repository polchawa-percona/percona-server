#
# The XA 2PC leg body of crashpoint_xa. The XA commands are issued as
# separate statements rather than a single multi-statement batch, so that
# an abort in the middle of the leg does not leave the client connection
# out of sync (a leftover multi-statement result would make
# include/wait_until_disconnected.inc fail with 2014).
#
# Parameters (set by the test):
#   $crashpoint_xa_xid     the XA xid of the leg
#   $crashpoint_xa_finish  the final "XA COMMIT 'xid'" or
#                          "XA ROLLBACK 'xid'"
#
--eval XA START '$crashpoint_xa_xid';
INSERT INTO cp.t VALUES (5, 50);
--eval XA END '$crashpoint_xa_xid';
--eval XA PREPARE '$crashpoint_xa_xid';
--eval $crashpoint_xa_finish;
