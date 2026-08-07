# LRU contention and scalability remediation

## As Is

The grouped-LRU branch reduces one part of promotion-drain contention, but
the dominant oversized-working-set paths still serialize substantial work on
`buf_pool_t::LRU_list_mutex`.

The current implementation has these known performance characteristics:

- A query thread that crosses `innodb_lru_make_young_drain_threshold` runs
  `buf_LRU_drain_promote_queue()` synchronously.
- Deferred promotion stores raw `buf_page_t *` values and buffer-fixes every
  queued page until the complete drain finishes, making those pages
  temporarily non-evictable.
- Promotion producers update several shared per-pool atomics; queue head,
  length, and drain arbitration occupy the same cache-line region.
- The deferred pass holds `LRU_list_mutex` while accounting for and
  re-appending the complete batch.
- Page insertion/removal still holds `LRU_list_mutex` while taking group
  mutexes, scanning up to 32 slots, and occasionally allocating group
  objects.
- LRU flush batching abandons the remainder of a group after its first
  eviction or flush dispatch and moves toward younger groups.
- Arbitrary promotion detaches leave sparse groups. Empty groups are cached
  for the lifetime of the buffer-pool instance, so peak fragmented group
  metadata remains resident across invalidation and pool shrink.
- Tail eviction still holds `LRU_list_mutex`; the intended group-only
  eviction phase is not implemented.
- `buf_lru_manager_adapt_sleep_time()` treats the number of groups as the
  number of pages.

Two recent commits already removed the per-drain O(all-groups) reclaim walk
and the per-mutation debug O(all-groups) validator. Those fixes must be
preserved.

## To Be

Deferred promotion is a background-only, bounded optimization. Query threads
enqueue `(page_id_t, residency_generation)` values without pinning
buffer-page descriptors and never execute a drain. The generation identifies
one page-hash residency, not merely a logical page number, and is preserved by
descriptor relocation/conversion but changed by eviction and reload.
Background processing resolves each identity under the page-hash S latch,
temporarily buffer-fixes only the matching current descriptor, releases the
hash latch, then revalidates and performs an ordinary promotion under
`LRU_list_mutex`. Stale, duplicate, and full-queue entries are harmless.
Processing is chunked so one activation cannot monopolize
`LRU_list_mutex`.

Grouped LRU mutations avoid redundant nested locking and linear free-slot
searches. Flush and eviction continue processing the oldest eligible group
instead of scattering one action across many groups. Sparse adjacent groups
of the same old/young class are compacted under a bounded background budget,
and retired group lifetime uses bounded reclamation rather than an unbounded
cache.

The final stage introduces identity-based optimistic tail scanning. A short
`LRU_list_mutex` plus group-mutex section snapshots stable
`(page_id_t, residency_generation)` candidates from an old tail group.
Eligibility is examined outside the list mutex, but eviction is committed
only after reacquiring and revalidating through the existing
LRU-list → page-hash → block → group order. No live page is detached from its
group before the conventional atomic eviction commit.

## Requirements

1. Foreground threads must never call `buf_LRU_drain_promote_queue()`.
   A threshold crossing must only signal background work.
2. Promotion enqueue must not increment `buf_fix_count` or retain a raw
   `buf_page_t *` whose lifetime extends beyond the enqueue call. Queue
   entries must contain `(page_id_t, residency_generation)`. The consumer
   must resolve under the confirmed page-hash S latch and temporarily fix the
   matching descriptor before releasing that latch.
3. The promotion queue must be bounded and preallocated. It must not depend on
   finite-tag ABA avoidance or allow a producer paused after reserving one
   slot to block consumption of other published slots. Busy-slot and duplicate
   entries may be dropped because promotion is heuristic, but they must not
   block query threads or corrupt page/group state.
4. Producer-side queue metadata must avoid placing independently contended
   atomics on one cache line. No `new`/`delete`, InnoDB mutex operation, or
   `LRU_list_mutex` acquisition is allowed in successful enqueue. An
   empty-to-nonempty background event signal is permitted.
5. The page-cleaner coordinator is the sole promotion consumer in normal,
   read-only, and recovery operation, independent of `innodb_lru_threads`.
   The recovery branch of that coordinator must use a promotion-aware wake
   plus periodic fallback because it does not universally wait on
   `buf_flush_event`. Shutdown and invalidation must close enqueue,
   synchronize with the consumer, discard or drain queued values, then
   reset/reopen as appropriate. Background work must have fixed queue
   capacity, dequeue chunk, temporary-fix count, chunks-per-activation, and
   time-slice/resignal behavior. Remaining work must be rescheduled without a
   foreground fallback.
6. Group append must use constant-time free-slot bookkeeping and at most one
   group-mutex acquisition for the common reusable-fill-group case.
   Steady-state group allocation and mutex creation must remain outside the
   common mutation path.
7. LRU flush batching must not permanently skip the rest of the oldest group
   after one operation releases and reacquires `LRU_list_mutex`. It must
   restart safely at that group or at the current tail.
8. Sparse adjacent groups with the same old/young classification must be
   compacted under a bounded background budget while preserving flattened
   page traversal order. The first implementation must skip `LRU_old`, active
   fill groups, and groups referenced by maintenance cursors. After producers
   quiesce, repeated bounded passes must converge so same-class mergeable
   adjacent pairs no longer remain, with only a fixed number of skipped
   partial groups.
9. After removal of every asynchronous raw group-pointer capture, group
   reclamation must use a bounded reusable reserve plus a retired list.
   Unreferenced overflow must be destroyed outside `LRU_list_mutex`, and
   invalidation/shrink must trim excess reserve entries. Group lifetime states
   and reuse generations must be explicit and mechanically validated.
10. Tail candidate discovery must not hold `LRU_list_mutex` for the complete
    eligibility scan. It may copy bounded identity candidates under a short
    list/group hold, but every eviction commit must reacquire and revalidate
    through the existing LRU-list → page-hash → block → group ordering.
    A hash-visible live page must never be group-detached with deferred global
    bookkeeping.
11. `buf_lru_manager_adapt_sleep_time()` must use `LRU_n_pages`, not the
    number of groups, when deriving the free-list target.
12. Existing observable LRU behavior and configuration must remain
    compatible: `innodb_old_blocks_pct`, `innodb_old_blocks_time`,
    `innodb_lru_make_young_drain_threshold`, buffer-pool resize/invalidation,
    recovery, compressed pages, AHI, and LRU manager/page-cleaner ownership.
    A zero drain threshold continues to select immediate promotion. A nonzero
    threshold is a soft background-wakeup target capped below the fixed queue
    capacity; values larger than capacity must not prevent wakeup or progress.
13. Add low-overhead counters sufficient to verify queue drops, background
    wakeups, drain batch sizes, maximum drain list-mutex duration, linked and
    cached group counts, compaction, scan work, and group occupancy.

## Acceptance Criteria

1. No foreground, DDL, resize, invalidation, or assertion call path can
   execute `buf_LRU_drain_promote_queue()`. Queue shutdown is coordinated by
   close-and-discard/drain with the background owner.
2. Enqueue contains no `buf_block_fix()` and no raw page pointer is stored in
   queue storage. A page evicted and reloaded while its old identity remains
   queued has a different generation and is not promoted by that entry.
   Consumer-side fixing is bounded by the dequeue chunk.
3. Queue capacity is fixed per buffer-pool instance; enqueue failure records a
   drop and returns immediately. Slot reuse has no linked-structure ABA
   dependency, and a producer paused before publication cannot corrupt or
   permanently stall other slots.
4. Queue producer ticket, wake state, and producer-path metrics are cache-line
   separated. Source inspection confirms enqueue performs one ticket
   increment, one bounded slot reservation, one value copy, publication, and
   at most one event signal.
5. Lost-wakeup tests cover enqueue racing with consumer wake-state clearing.
   Each activation obeys fixed entry/chunk/fix/time budgets, and backlog
   causes another background wake rather than foreground execution. Recovery,
   read-only, both `innodb_lru_threads` modes, shutdown, and invalidation have
   explicit queue ownership/lifecycle tests.
6. The common append path performs one group lock and uses a bitmap/cursor or
   equivalent constant-time slot selection. Cached group reuse avoids
   allocation and mutex creation.
7. After a successful flush/eviction invalidates a group pointer, the batch
   safely restarts from the current tail or revalidates the same group; it
   does not unconditionally advance to the predecessor captured before the
   operation.
8. A deterministic quiescent-churn test demonstrates that bounded compaction
   removes all eligible same-class adjacent pairs while preserving flattened
   order, old/young flags, counters, fill pointers, and hazard pointers.
9. No path retains a raw group pointer after releasing all of
   `LRU_list_mutex`, the group mutex, and an explicit lifetime reference.
   Invalidation/shrink releases excess retired/reserve groups, with mutex
   destruction outside `LRU_list_mutex`.
10. A tail scan copies at most one group's identity candidates in a short
    list/group section, then drops the list mutex for provisional inspection.
    Every successful eviction uses the conventional commit order and
    revalidates generation, page-hash identity, current group membership,
    cleanliness, fix count, and I/O state.
11. The manager sleep calculation reads `buf_pool->LRU_n_pages`.
12. Existing focused MTR tests remain logically compatible, and new tests
    cover background-only promotion, queue overflow/staleness, group
    compaction, invalidation, and tail-scan concurrency.
13. Metrics distinguish enqueue, drop, wake, drain, compaction, occupancy,
    and eviction-scan behavior without adding a contended global counter to
    the foreground page-access path.

The 80-CPU sysbench OLTP RW performance run is explicitly a user-owned
post-implementation gate. This task will prepare the code and measurement
counters but will not claim TPS acceptance without that external run.

## Testing Plan

1. Add a standalone gunit test for the preallocated fixed-slot queue:
   enqueue/dequeue, overflow, producer pause before publication, duplicate
   identity, wake reset/recheck, and concurrent producers/consumer.
2. Add residency-generation tests covering fresh read, eviction/reload,
   descriptor relocation, and block-to-compressed-only conversion.
3. Add a debug test proving threshold crossing signals background work and
   never drains on the foreground thread.
4. Extend grouped-LRU debug validation for slot bitmap/count consistency,
   group lifetime state, cache/retired lists, compaction, and old/young
   classification.
5. Add a deterministic sparse-group compaction test with stable page count
   followed by producer quiescence.
6. Add a flush-batch test in which the first tail-group operation releases
   `LRU_list_mutex`; verify the batch resumes at the oldest valid position.
7. Add concurrent promotion/eviction/invalidation stress using low queue and
   chunk thresholds, suitable for debug, ASAN, and TSAN builds.
8. Add lifecycle tests for normal/read-only operation, recovery wake,
   force-recovery/log-test variants, shutdown phase transitions, both
   `innodb_lru_threads` values, and invalidation with queued entries.
9. Run the narrow existing tests when a configured build is available:
   `sys_vars/innodb_lru_make_young_drain_threshold_basic`,
   `sys_vars/innodb_lru_threads_basic`,
   `innodb_zip/lru_mutex_narrow_stress_debug`,
   `percona_innodb/lru_flusher_debug`, and
   `innodb/innodb_lru_threads_kill_recovery`.
10. Run source-level formatting, diff, and lock-order reviews after every
   stage. This worktree currently has no configured build directory, so
   executable build/MTR or sanitizer results must pass in configured CI before
   merge but are not claimed by this local code-only gate.
11. Hand off counter definitions and an exact external sysbench matrix for
   64/128/256/512 threads, 12 GiB buffer pool, and 24 GiB database.

## Implementation Plan

1. Fix the manager page-count calculation and flush-group resumption first;
   add focused debug hooks/counters and source-review their oracles.
2. Add the standalone preallocated fixed-slot value queue and gunit tests,
   without integrating it into the buffer pool.
3. Add a monotonic per-pool residency generation and preserve it across
   descriptor relocation/conversion; test reload ABA behavior.
4. Integrate `(page_id_t, generation)` enqueue and a hash-S-latched consumer
   that temporarily fixes only resolved matching descriptors. Use ordinary
   LRU promotion and remove the raw-pointer queue, per-page queue link/flag,
   group-only promotion detach, deferred counter window, and obsolete
   `LRU_drain_mutex` scaffolding once no caller needs them.
5. Make the page-cleaner coordinator the sole consumer in normal, read-only,
   and recovery phases. Make its recovery wait promotion-wake-aware with a
   periodic fallback, and implement close, drain/discard, reset, and reopen
   protocols for shutdown/invalidation. Remove every
   foreground/DDL/resize/assertion synchronous drain.
6. Add fixed dequeue/chunk/fix/time budgets, reliable wake-state rechecking,
   threshold-to-capacity capping, and rescheduling. Cache-line-separate queue
   coordination and validate lifecycle/adversarial queue tests.
7. Add constant-time group free-slot bookkeeping and collapse the common
   append path to one group lock; validate slot/count invariants.
8. Replace the unbounded cache with a small reusable reserve plus retired-list
   reclamation now that no asynchronous raw group captures remain. Destroy
   overflow outside `LRU_list_mutex`; validate invalidation and shrink.
9. Add bounded, exact-order-preserving compaction of eligible adjacent
   same-class groups; validate quiescent convergence and all group invariants.
10. Stop for a correctness review gate: inspect lock ordering, run every
    available narrow test, and review all new metrics before starting the
    eviction rewrite.
11. Implement identity-based optimistic tail candidate scanning: snapshot one
    group's `(page_id, generation)` values in a short list/group section,
    inspect provisionally outside the list mutex, and commit each eviction
    only through existing LRU-list → hash → block → group ordering after full
    revalidation. Never perform group-only detach.
12. Remove superseded compatibility scaffolding, update sysvar/comments and
    test expectations, run final source checks, and prepare the external
    sysbench command/counter checklist.
