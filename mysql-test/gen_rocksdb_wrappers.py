#!/usr/bin/env python3
"""Generate *_rocksdb wrappers from *_innodb engine-parametrised tests.

Usage: gen_rocksdb_wrappers.py <mysql-test dir>   (prints a TSV report)
"""
import os
import re
import sys

root = sys.argv[1]
HDR = "--source include/have_rocksdb.inc\n"
OPT = ("$ROCKSDB_OPT $ROCKSDB_LOAD_ADD --loose-rocksdb_lock_wait_timeout=1 "
       "--loose-rocksdb_force_compute_memtable_stats_cachetime=0\n")
# Not ported: tests of InnoDB internals, or tests whose shared .inc relies on a
# feature MyRocks does not support (the .inc files are upstream and stay untouched).
SKIP = {
    "partition_basic_symlink_innodb": "InnoDB DATA DIRECTORY / file_per_table / strict mode",
    "partition_innodb_status_file": "InnoDB status file and monitor thread",
    "partition_reorganize_innodb": "InnoDB tablespaces and I_S.INNODB_* tables",
    "partition_mgm_lc0_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_mgm_lc1_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_mgm_lc2_innodb": "EXCHANGE PARTITION; also needs lower_case_table_names=2",
    "partition_exchange_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_exch_myisam_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition-dml-1-4-innodb": "SERIALIZABLE isolation (unsupported by MyRocks)",
    "partition-dml-1-6-innodb": "READ UNCOMMITTED isolation (unsupported by MyRocks)",
    "partition-dml-1-11-innodb": "DATA DIRECTORY per partition (unsupported by MyRocks)",
    "partition-dml-1-12-innodb": "HANDLER statements (unsupported by MyRocks)",
    "innodb_trig_frkey": "FOREIGN KEY (unsupported by MyRocks)",
    "partition_debug_sync_innodb": "waits on sync point alter_table_inplace_before_commit; MyRocks REORGANIZE PARTITION is COPY-only (even with rocksdb_allow_unsafe_alter)",
    "partition_value_innodb": "the InnoDB original is itself skipped unconditionally (--skip at top)",
    "partition-dml-1-3-innodb": "pins global REPEATABLE READ to test partition-scoped locking reads (gap locks)",
    "partition-dml-1-7-innodb": "pins global REPEATABLE READ to test partition-scoped locking reads (gap locks)",
    "partition-dml-1-8-innodb": "pins global REPEATABLE READ to test partition-scoped locking reads (gap locks)",
    "partition-dml-1-10-innodb": "pins global REPEATABLE READ to test partition-scoped locking reads (gap locks)",
    "part_exch_valid_hash_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "part_exch_valid_key_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "part_exch_valid_list_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "part_exch_valid_range_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_exch_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_exch_qa_1_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_exch_qa_4_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_exch_qa_5_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_exch_qa_7_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
    "partition_exch_qa_8_innodb": "EXCHANGE PARTITION (no HA_CAN_EXCHANGE_PARTITION in MyRocks)",
}
# MyRocks has no gap locks; at REPEATABLE READ the server rejects locking reads
# without a full unique key in multi-statement/multi-table statements. These
# tests run their scenario at READ COMMITTED instead.
READ_COMMITTED = {
    "part_exch_valid_hash", "part_exch_valid_key", "part_exch_valid_list",
    "part_exch_valid_range", "partition_alter1_1_2", "partition_alter1_1",
    "partition_alter1_2", "partition_alter2_1_1", "partition_alter2_1_2",
    "partition_alter2_2_1", "partition_alter2_2_2", "partition_alter4",
    "partition_basic", "partition_debug_sync", "partition-dml-1-10",
    "partition-dml-1-3", "partition-dml-1-7", "partition-dml-1-8",
    "partition_engine", "partition_exch_qa_1", "partition_exch_qa_4",
    "partition_exch_qa_5", "partition_exch_qa_7", "partition_exch_qa_8",
    "partition_exch", "part_supported_sql_func",
    "innodb_storedproc_10", "innodb_trig_0102", "innodb_trig_03e",
    "innodb_trig_08", "innodb_views",
}
# Per-test source lines dropped from the wrapper (feature unsupported by MyRocks).
DROP_LINES = {
    "partition_auto_increment_innodb": ["--source suite/parts/inc/partition_auto_increment_import.inc"],
}
RPL = {"rpl-partition-dml-1-1-innodb"}
# Extra server options per test. partition_alter3 prints EXPLAIN row estimates;
# MyRocks estimates vary between runs, so pin them (pattern used by ~29 rocksdb tests).
EXTRA_OPT = {
    "partition_alter3_innodb": "--rocksdb_debug_optimizer_n_rows=1000",
    # The semijoin plan of one view query depends on the row estimates, and
    # the plan decides which row a deprecation warning reports ("at row N").
    "innodb_func_view": "--rocksdb_debug_optimizer_n_rows=1000",
}


def conv(text):
    t = re.sub(
        r"(let \$(?:engine|ENGINE|engine_table|engine_part|engine_subpart)\s*=\s*)('?)(?:InnoDB|INNODB|innodb)'?",
        lambda m: m.group(1) + m.group(2) + "ROCKSDB" + m.group(2), text)
    t = re.sub(r"let \$engine_type\s*=\s*innodb;", "let $engine_type= rocksdb;", t)
    t = re.sub(r"(ENGINE\s*=\s*)InnoDB\b", r"\1ROCKSDB", t, flags=re.I)
    t = re.sub(r"(SET\s+default_storage_engine\s*=\s*)InnoDB", r"\1ROCKSDB", t, flags=re.I)
    t = re.sub(r"suite/funcs_1/include/innodb_tb(\d)\.inc", r"suite/funcs_1/include/rocksdb_tb\1.inc", t)
    t = re.sub(r"let \$type\s*=\s*'InnoDB'\s*;", "let $type= 'ROCKSDB' ;", t)
    return t


# funcs_1 table definitions: copies of innodb_tbN.inc using MyRocks (same data files).
inc = os.path.join(root, "suite", "funcs_1", "include")
for n in "1234":
    s = open(os.path.join(inc, "innodb_tb%s.inc" % n)).read()
    s = s.replace("##### suite/funcs_1/include/innodb_tb%s.inc" % n,
                  "##### suite/funcs_1/include/rocksdb_tb%s.inc (copy of innodb_tb%s.inc using MyRocks)" % (n, n), 1)
    s = s.replace("engine = innodb;", "engine = rocksdb;")
    open(os.path.join(inc, "rocksdb_tb%s.inc" % n), "w").write(s)


out = []
for suite, pat in (("parts", r".*innodb.*\.test$"), ("funcs_1", r"^innodb_.*\.test$")):
    tdir = os.path.join(root, "suite", suite, "t")
    for f in sorted(os.listdir(tdir)):
        if not re.match(pat, f):
            continue
        base = f[:-5]
        if base in SKIP:
            out.append((suite, base, "", "not ported: " + SKIP[base]))
            continue
        new = base.replace("innodb", "rocksdb")
        body = conv(open(os.path.join(tdir, f)).read())
        for drop in DROP_LINES.get(base, []):
            body = body.replace(drop + "\n", "")
        lines = body.splitlines(keepends=True)
        i = 0
        while i < len(lines) and (lines[i].startswith("#") or not lines[i].strip()):
            i += 1
        stem = re.sub(r"[-_]innodb$", "", base)
        header = "# Generated from %s.test: runs the same scenario on MyRocks.\n" % base
        if stem in READ_COMMITTED:
            header += "# Runs at READ COMMITTED (see -master.opt): MyRocks has no gap locks.\n"
        if base in EXTRA_OPT:
            header += "# Extra server options (see -master.opt): %s\n" % EXTRA_OPT[base]
        if base in DROP_LINES:
            header += "# Omitted (unsupported by MyRocks): %s\n" % ", ".join(DROP_LINES[base])
        body = "".join(lines[:i]) + header + HDR + "".join(lines[i:])
        open(os.path.join(tdir, new + ".test"), "w").write(body)
        opt = OPT
        if stem in READ_COMMITTED:
            opt = opt.rstrip("\n") + " --transaction-isolation=READ-COMMITTED\n"
        if base in EXTRA_OPT:
            opt = opt.rstrip("\n") + " " + EXTRA_OPT[base] + "\n"
        mo = os.path.join(tdir, base + "-master.opt")
        if os.path.exists(mo):
            extra = " ".join(o for o in open(mo).read().split() if "innodb" not in o.lower())
            if extra:
                opt = opt.rstrip("\n") + " " + extra + "\n"
        open(os.path.join(tdir, new + "-master.opt"), "w").write(opt)
        if base in RPL:
            open(os.path.join(tdir, new + "-slave.opt"), "w").write(opt)
        left = [l.strip() for l in body.splitlines()
                if re.search(r"innodb", l, re.I) and not l.startswith("#")]
        out.append((suite, base, new, "; ".join(left)[:200]))

for r in out:
    print("\t".join(r))
