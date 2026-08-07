# LRU audit remediation

## As Is

The grouped-LRU branch has correctness and performance gaps remaining after
commit `ff43f2f2ee2` and its follow-up fixes:

- repeated accesses can enqueue the same page residency many times, allowing
  one hot page to occupy the bounded promotion queue and replay old requests;
- one promotion drain takes `LRU_list_mutex` once per identity and performs
  repeated old-boundary maintenance;
- the bounded MPSC consumer, empty check, and wake-reset handshake scan all
  queue slots;
- promotion backlog re-signals the global page-cleaner event after every small
  chunk, causing repeated coordinator-wide work;
- background group-cache maintenance can destroy or allocate an unbounded
  number of groups while holding the promotion lifecycle rendezvous mutex;
- every grouped-LRU mutation still takes a group mutex while already holding
  `LRU_list_mutex`, although no current mutation path changes group contents
  without the list mutex;
- an LRU flush batch restarts from the global tail whenever an operation drops
  `LRU_list_mutex`, repeatedly rescanning previously examined tail groups;
- enabled per-pool LRU managers sleep on a non-interruptible timer, so
  foreground free-list pressure can wait up to one second before background
  LRU work starts.

## To Be

Each page-hash residency has at most one deferred promotion outstanding.
The sole consumer resolves and temporarily pins a bounded chunk, then applies
all still-valid promotions in one bounded `LRU_list_mutex` section with one
final old-boundary adjustment. Queue publication and consumption remain
lock-free on the producer path and constant-time per consumed item, including
the empty/wakeup handshake.

Promotion and compaction maintenance use a coordinator-level time/work budget.
Backlog causes at most one reschedule after that budget, not one full
page-cleaner iteration per drain chunk. Group reserve maintenance performs
only fixed work in a normal background activation, so invalidation and
shutdown rendezvous latency is bounded.

`LRU_list_mutex` is the sole lock protecting group membership, slots,
occupancy, classification, and list links. LRU flush scanning protects and
resumes from the current group (or its adjusted predecessor if reclaimed)
instead of returning to the global tail. A per-pool work event interrupts an
enabled LRU manager's adaptive sleep when foreground allocation observes an
empty free list.

## Requirements

1. Suppress duplicate deferred promotions per `(page_id,
   residency_generation)` with a fixed per-pool table keyed by the pool-unique
   residency generation. The table must not enlarge every `buf_page_t`, and
   the producer must not allocate, wait, or take an InnoDB mutex. Failed queue
   publication must release suppression. Descriptor relocation naturally
   retains the same generation, while a fresh residency receives a new key.
2. Drain at most `BUF_LRU_PROMOTE_DRAIN_CHUNK` identities per chunk. Resolve
   and buffer-fix candidates before entering `LRU_list_mutex`, apply the valid
   chunk under one list-mutex acquisition, and release all temporary fixes on
   every path. Old/young boundary maintenance must run at most once for a
   mature-list batch.
3. Make successful queue consumption, `empty()`, and wake-reset readiness
   checks O(1). A producer paused after slot reservation must not block
   publication or consumption of other producers, and queue correctness must
   not rely on a finite ABA tag.
4. Bound promotion/compaction work per page-cleaner activation by both fixed
   item budgets and a small elapsed-time budget. Do not signal
   `buf_flush_event` once per pool or once per chunk; if work remains after the
   activation budget, arm at most one follow-up wake. Continue fair per-pool
   rounds while time remains. A compaction retry blocked only by a live hazard
   remains pending for a later periodic or external wake and must not cause an
   immediate self-wake loop.
5. Bound normal group-cache maintenance. Each background activation may steal,
   destroy, and create only fixed numbers of groups. Startup, invalidation,
   and quiescent teardown may explicitly request exhaustive maintenance.
6. Remove the redundant per-group mutex and its latch registration. All
   accesses and mutations of group slots, counts, classification, and
   membership must assert or otherwise require `LRU_list_mutex`.
7. After an LRU flush/eviction operation drops and reacquires
   `LRU_list_mutex`, resume from a hazard-protected current group if it
   survived, or from the hazard pointer's adjusted predecessor if it was
   reclaimed. Never restart unconditionally from the global tail.
8. Add a per-pool LRU-manager work event. Foreground allocation must signal it
   after observing that the free list is empty while dedicated LRU managers
   own LRU flushing. The manager's adaptive sleep must be interruptible by
   that event and remain compatible with invalidation parking and shutdown.
9. Preserve page-hash residency ABA safety, promotion close/reopen behavior,
   source-gated LRU flushing, optimistic tail-scan ownership, sparse
   compaction convergence, compressed-page handling, and lock ordering.

## Acceptance Criteria

1. Concurrent accesses to one old residency produce at most one queued item
   until that item is consumed or publication fails. A stale consumer clears
   only its exact generation tag. Eviction/reload and descriptor relocation
   tests prove that stale identities cannot clear or suppress a new
   residency's request.
2. Instrumented/source-level checks show one list-mutex acquisition per
   promotion chunk and no leaked temporary fix when identities are missing,
   stale, young, or invalid. A mature chunk performs at most one old-boundary
   adjustment.
3. Queue tests prove constant-time readiness through counters/hooks rather
   than slot scans, and retain overflow, paused-producer, wake-race, and MPSC
   stress coverage.
4. One coordinator activation obeys its promotion chunk and elapsed-time
   limits across all buffer-pool instances, continuing fair rounds while time
   remains. Runnable work produces one follow-up signal, while completed work
   and hazard-only compaction retry produce none.
5. Background maintenance never creates or destroys more than its configured
   per-activation budgets while holding `LRU_drain_mutex`; lifecycle paths
   still empty or replenish the reserve as required.
6. No `buf_lru_group_t` mutex, `LATCH_ID_BUF_POOL_LRU_GROUP`, or
   `SYNC_BUF_LRU_GROUP` reference remains. Debug validation confirms bitmap,
   count, back-pointer, classification, and flattened page-count invariants
   under `LRU_list_mutex`.
7. A batch that dispatches several dirty tail pages examines each surviving
   tail group only a bounded number of times and continues toward younger
   groups without dereferencing reclaimed memory.
8. A foreground empty-free-list observation wakes the corresponding enabled
   LRU manager promptly. Timed idle behavior, disabled ownership, invalidation,
   debug parking, and shutdown still terminate correctly.
9. Focused unit tests and available InnoDB build targets pass; formatting and
   lock-order review report no new issue. Full MTR, sanitizer, and
   64/128/256/512-thread sysbench validation remain external gates when the
   local build/test environment does not provide them.

## Testing Plan

1. Extend `bounded_mpsc_queue-t` first with tests for O(1) published-list
   consumption/readiness, paused reservation, wake-clear races, slot reuse,
   overflow, and concurrent producers.
2. Add a small unit-testable fixed generation-dedup helper or focused debug
   hooks, then cover first enqueue, collision/drop behavior, duplicate
   suppression, full-queue rollback, consume/re-enqueue, relocation, and a
   fresh residency generation.
3. Add debug counters/assertions for one promotion list-mutex section per
   chunk, fixed candidates, balanced fixes, one mature boundary adjustment,
   coordinator activation budgets, and bounded group maintenance.
4. Add or extend a deterministic LRU flush debug test so a tail group survives
   one flush dispatch and scanning resumes from that group rather than the
   global tail; include group reclamation during the unlocked window.
5. Add a debug hook/MTR assertion for interruptible LRU-manager sleep after
   foreground free-list pressure, plus OFF/ON ownership, invalidation, and
   shutdown coverage.
6. Build and run the narrowest available targets after each stage:
   `bounded_mpsc_queue-t`, `exclusive_scan-t`, and `innobase`. Then run
   `sys_vars/innodb_lru_make_young_drain_threshold_basic`,
   `sys_vars/innodb_lru_threads_basic`,
   `innodb_zip/lru_mutex_narrow_stress_debug`,
   `percona_innodb/lru_flusher_debug`, and
   `innodb/innodb_lru_threads_kill_recovery` when configured.
7. Run clang-format checks, focused source searches, and final diff/lock-order
   review. Do not claim external TPS improvement without the user-owned
   sysbench matrix.

## Implementation Plan

1. Replace the queue's scanning consumer with a published-ready index list;
   add the queue tests first and run `bounded_mpsc_queue-t`.
2. Add the fixed per-pool residency-generation dedup table and exact-tag clear
   behavior; add the smallest focused test/debug coverage, then build the
   affected buffer-pool code.
3. Refactor promotion drain into resolve/pin and single-list-mutex apply
   phases, with one mature boundary adjustment; run queue tests and the
   narrowest InnoDB build.
4. Add coordinator-wide promotion/compaction time budgeting and single
   follow-up wake behavior; verify recovery, debug-disabled, normal, and
   read-only call sites.
5. Add explicit bounded and exhaustive group-cache maintenance modes; verify
   startup, invalidation, compaction, drain, and teardown call sites.
6. Remove group mutex operations, storage, and latch registration; strengthen
   `LRU_list_mutex` assertions and run focused source and build checks.
7. Change LRU flush traversal to hazard the current group and resume from its
   adjusted value after unlocked operations; add debug coverage and build.
8. Add, initialize, destroy, signal, and wait on the per-pool LRU-manager work
   event; test enabled/disabled, invalidation, and shutdown paths.
9. Run all available focused tests/builds, clang-format, lints, and a final
   correctness/performance review.
