# LRU Contention Remediation — External Handoff

## Scope completed in-tree

Stages 1–11 of `.requirements/20260807T062533Z_lru_contention_remediation/REQUIREMENTS.md`:

1. LRU manager page-count / flush-group resumption
2. Bounded MPSC promotion queue + gunit tests
3. Residency generation (ABA-safe identities)
4–5. Background-only promotion consumer + lifecycle/recovery wake
6. Constant-time group slot bitmap / one-lock append
7. Bounded group reserve + retired destruction
8. Sparse adjacent-group compaction
9. Correctness review gate (invalidate/teardown race fixed)
10. Identity-based optimistic tail eviction scan
11. Comment/sysvar alignment + this handoff

## Local verification already done

- `innobase` Debug rebuild succeeds after Stages 6–10.
- `bounded_mpsc_queue-t`: 7/7 tests pass.
- `git diff --check` clean on touched sources.

## Recommended MTR (configured CI / full tree)

- `sys_vars.innodb_lru_make_young_drain_threshold_basic`
- `sys_vars.innodb_lru_threads_basic`
- `innodb_zip.lru_mutex_narrow_stress_debug`
- `percona_innodb.lru_flusher_debug`
- `innodb.innodb_lru_threads_kill_recovery`

## External sysbench gate (user-owned)

Machine context from the original regression: ~80 CPUs; compare branch vs `8.4` baseline.

```bash
# Prepare (once)
sysbench oltp_read_write \
  --mysql-host=127.0.0.1 --mysql-user=... --mysql-password=... \
  --mysql-db=sbtest --tables=8 --table-size=10000000 \
  prepare

# Run matrix (BP=12G, DB≈24G). Repeat for threads in 64 128 256 512.
sysbench oltp_read_write \
  --mysql-host=127.0.0.1 --mysql-user=... --mysql-password=... \
  --mysql-db=sbtest --tables=8 --table-size=10000000 \
  --threads=${THREADS} --time=300 --report-interval=10 \
  run
```

Suggested mysqld knobs for the comparison:

- `innodb_buffer_pool_size=12G`
- keep other InnoDB settings matched to the baseline run that showed the 3× regression
- exercise both `innodb_lru_make_young_drain_threshold=0` and a nonzero value (e.g. `128`)

Acceptance: higher TPS than the pre-remediation branch at 64/128/256/512 threads under the same BP/DB sizes; no correctness failures under debug + sync-debug if available.

## Observability to watch during the run

From existing monitors / status (names may vary by build):

- LRU search scanned cumulative (`MONITOR_LRU_SEARCH_SCANNED*`)
- Free-list / LRU get-free loops
- Page-cleaner activity and flush event wake rate
- Buffer pool hit rate and dirty-page counts

Queue-specific counters (enqueue drops, drain batch size, compact merges) are still thin; if the sysbench gate is inconclusive, add non-contended instance counters next rather than foreground atomics on the access path.

## Push target

Feature work for this effort belongs on `pawel/PS-11141-8.4-lru-groups` (never `origin` unless explicitly requested).
