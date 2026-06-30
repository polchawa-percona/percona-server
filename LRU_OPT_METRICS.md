# LRU optimization metrics

Monitor counters added to instrument the `lru-perf-patches-2` buffer-pool
optimizations, split across per-optimization branches cut from this baseline
(`lru-opt-baseline`).

All new counters default **OFF** (`MONITOR_NONE`) — they add a per-event
branch in hot buffer-pool paths, so leave them off in production and enable
only while measuring.

```sql
-- enable (this also enables the related upstream buffer_LRU_* counters)
SET GLOBAL innodb_monitor_enable = 'buffer_LRU_%';

-- read
SELECT name, count, count_reset, avg_count, status
FROM   information_schema.innodb_metrics
WHERE  name LIKE 'buffer_LRU_%'
ORDER  BY name;

-- reset between A/B runs / disable
SET GLOBAL innodb_monitor_reset_all = 'buffer_LRU_%';
SET GLOBAL innodb_monitor_disable   = 'buffer_LRU_%';
```

## Free-page search — "why couldn't this scanned page be freed?"

Optimization-independent; present on **every** branch (defined here on the
baseline) so all branches are mutually comparable. Counted per
scanned-but-not-freed page in the common-LRU free search
(`buf_LRU_free_from_common_LRU_list`).

| Counter | Type | Meaning |
|---|---|---|
| `buffer_LRU_scan_skip_pinned`   | counter | not freed — buffer-fixed (pinned by another thread) |
| `buffer_LRU_scan_skip_io_read`  | counter | not freed — a read I/O was in progress |
| `buffer_LRU_scan_skip_dirty`    | counter | not freed — **dirty, not yet being flushed** |
| `buffer_LRU_scan_skip_flushing` | counter | not freed — **dirty, flush already in progress** |
| `buffer_LRU_scan_skip_other`    | counter | not freed — uncategorized (stale / unzip paths); reconciliation bucket |

Derived: **dirty total** = `skip_dirty + skip_flushing`; **of dirty, already
flushing** = `skip_flushing`; **other reasons** = `skip_pinned + skip_io_read`.
The five buckets reconcile to `(scanned − freed)`.

*How often / how deep* are already covered by the upstream counters
`buffer_LRU_get_free_search`, `buffer_LRU_get_free_loops`,
`buffer_LRU_get_free_waits`, and `buffer_LRU_search_scanned` / `_per_call`.

## O1 — deferred make-young (LRU promote) queue

Branch: `lru-opt-o1-make-young` · sysvar `innodb_lru_make_young_drain_threshold`
(default 0 = off; non-zero engages the deferred queue → built-in A/B).

| Counter | Type | Meaning |
|---|---|---|
| `buffer_LRU_make_young_calls`           | counter | too-old-page promotions on **both** paths — shared A/B denominator |
| `buffer_LRU_promote_enqueued`           | counter | pages pushed onto the lock-free deferred queue |
| `buffer_LRU_promote_skip_in_queue`      | counter | promotions elided because the page was already queued |
| `buffer_LRU_promote_skip_draining`      | counter | threshold crossings that found a drain already in progress |
| `buffer_LRU_promote_queue_len`          | gauge   | current queue depth |
| `buffer_LRU_promote_drain_pages`        | counter | total pages promoted by drains (set owner) |
| `buffer_LRU_promote_drain_num_call`     | counter | number of drains performed |
| `buffer_LRU_promote_drain_pages_per_call` | gauge | **pages per drain = `LRU_list_mutex` acquisitions amortised** (headline) |
| `buffer_LRU_promote_drain_lru_mutex_us` | counter (µs) | total time the drain held `LRU_list_mutex` |

## O2 — single-page-flush concurrency cap

Branch: `lru-opt-o2-single-page-flush` · sysvar
`innodb_single_page_flush_max_concurrent` (default UINT32_MAX = effectively
uncapped; set low to engage → built-in A/B).

| Counter | Type | Meaning |
|---|---|---|
| `buffer_LRU_single_page_flush_issued` | counter | user thread issued its own single-page LRU flush |
| `buffer_LRU_single_page_flush_capped` | counter | user thread waited for an in-progress flush instead, because the cap was reached |

## O3 / O4 / O5

These branches add no optimization-specific counters; measure them with the
free-search taxonomy above plus throughput/latency:

| Branch | Optimization | Toggle |
|---|---|---|
| `lru-opt-o3-init-read-narrow` | narrowed `LRU_list_mutex` in `buf_page_init_for_read` | none (always on; ⚠️ **non-compressed tables only**) |
| `lru-opt-o4-manager-thread`   | per-instance LRU manager thread                      | none (always on) |
| `lru-opt-o5-flush-batch`      | batched LRU eviction flush                           | `innodb_lru_flush_batch_size` (1 = off) |

## Measurement recipe

For the sysvar-gated optimizations (O1, O2, O5), run the same workload twice on
the **same** branch flipping only the knob — the deltas are the isolated effect
(no contamination from other always-on optimizations). For O3 and O4, compare
the branch against `lru-opt-baseline`.
