# Two-level grouped-LRU locking

## As Is

The grouped LRU stores up to `BUF_LRU_GROUP_SIZE` pages per
`buf_lru_group_t`, but `buf_pool_t::LRU_list_mutex` remains the sole lock for
group topology, slots, occupancy, page back-pointers, old/young
classification, counters, and fill pointers. Deferred promotion removes the
foreground make-young acquisition and resolves identities before locking, but
the consumer still applies a complete chunk under one exclusive list-mutex
section. Fresh insertion, eviction commit, removal, flush traversal,
compaction, and old-boundary adjustment also serialize on the mutex.

The earlier group-only promotion path detached hash-visible pages before
global accounting and reinsertion, retained unsafe asynchronous group/page
pointers, and depended on an unbounded group cache for lifetime. It was
replaced by the current identity, generation, temporary-fix, and conventional
commit protocol. Removing the then-redundant group mutex reduced nested lock
cost, but also left the branch without the original cross-group concurrency
goal.

## To Be

The global LRU lock becomes a topology read/write latch. Its shared mode
stabilizes linked group identity, list position, boundary/fill pointers, and
group lifetime while operations mutate independent groups under per-group
mutexes. Its exclusive mode is reserved for structural operations: publishing
or unlinking groups, changing fill pointers, moving the old/young boundary,
invalidation, and exhaustive validation.

Deferred promotion uses one private detached staging group owned by the sole
consumer. The drain chunk is capped at `BUF_LRU_GROUP_SIZE`, so one activation
can produce at most one full or partial staging group. Each valid, temporarily
fixed page is removed immediately from its source group under topology-S plus
its block/group locks and placed directly in the staging group. At the end of
the bounded chunk, the staging group is linked at the young head in O(1) under
topology-X, even when partial. Pages are never pointer-only or homeless. Empty
source groups are collected as referenced, generation-tagged candidates and
revalidated/unlinked in the same bounded topology-X publication phase.

Fresh insertion appends under topology-S and the active fill-group mutex.
Creating/linking a replacement fill group requires topology-X only when the
current group is absent, full, or reclassified. Eviction/removal detaches the
page immediately under topology-S plus the existing hash/block and group
locks; empty linked groups are queued for bounded topology-X reclamation.

Exact page-count deltas and formerly global hot-path statistics/signals are
atomic or separately protected. Old-sublist deltas are applied per page;
boundary movement and group reclassification are performed once per batch
under topology-X with a fixed group-step budget and background convergence.
Compressed/unzip-LRU bookkeeping has a separate ordered latch. Hash-visible
descriptor conversion preserves the page's existing group slot rather than
removing and reinserting it.

## Requirements

1. Replace the global LRU mutex role with an instrumented InnoDB topology
   `rw_lock_t`. Use S mode only to stabilize topology/lifetime for
   non-structural group operations and X mode for list links, group lifetime
   transitions, fill-pointer replacement, boundary movement, invalidation,
   and exhaustive validation. Do not use a sharded RW lock: group
   create/reclaim makes X acquisition too frequent for an all-shards X path.
   All topology acquisitions are non-recursive: S→S, X→X, X→S, S→X upgrade,
   and hidden recursive helper acquisition are forbidden. Wrappers assert that
   the caller owns neither mode before locking and provide explicit S-owned,
   X-owned, and S-or-X assertions for protected operations.
2. Reintroduce an instrumented mutex in `buf_lru_group_t`, with a latch level
   below page block/zip mutexes. `LRU_drain_mutex` remains above the topology
   latch, and the group mutex is always the innermost/last acquired latch.
   Resolve and pin under a page-hash latch, release the hash latch, and only
   then acquire topology; no thread may wait for topology while holding hash,
   block/zip, or group mutexes. Preserve only the audited existing
   frame-X-before-topology exception, while retaining the rule that no thread
   may wait for a frame latch while holding topology. No group holder may
   acquire or wait for topology, hash, block/zip, or frame latches. Multiple
   group locks must use one deterministic order.
3. Make group lifetime explicit (`linked`, `staging`, `reserve`, `retired`)
   and generation-tagged. Topology-S prevents unlink/reuse while a group
   pointer is acquired and locked. Any pointer surviving a topology-latch
   release must be protected by an explicit reference or a registered atomic
   hazard slot containing pointer plus `reuse_generation`, and must be
   revalidated after publication. Existing non-atomic shared hazard objects
   must not be concurrently written by topology-S holders. Invalidation drains
   or cancels all references/hazard registrations before group destruction.
4. Promotion enqueue remains allocation-free, mutex-free, nonblocking,
   identity-based, and deduplicated. The sole consumer resolves and
   temporarily fixes candidates as today.
5. Set `BUF_LRU_PROMOTE_DRAIN_CHUNK` to `BUF_LRU_GROUP_SIZE`. Maintain one
   drain-owned private staging group, allocated/replenished outside topology
   and group locks rather than taken from the shared reserve during a drain.
   For each valid page, acquire topology-S, its block mutex, and source-group
   mutex; revalidate identity/membership; atomically move it from the source
   group into the private staging group; update exact old-page deltas and the
   page's young classification; and release the locks. The page must always
   have a valid `lru_group` and slot. The staging group is
   single-consumer-owned and its pages remain temporarily fixed until
   publication. Scope-guard every temporary fix/reference and staging exit.
6. At the end of every bounded promotion chunk, acquire topology-X once,
   publish the one non-empty staging group at the young head in O(1),
   revalidate and unlink at most the fixed number of referenced empty-source
   candidates collected by the chunk, perform at most a fixed number of
   old-boundary group steps, and release X. Publish a partial group; do not
   retain it detached across activations. Release all temporary page fixes only
   after publication. Replenish the private staging group outside the latch.
   If an empty source cannot be reclaimed because it is active, boundary, or
   hazard-protected, transfer its existing maintenance reference and pending
   ownership into the global candidate queue before releasing local ownership;
   queue overflow transfers ownership to the persistent fallback scan.
7. Fresh page insertion appends to a linked old-side fill group under
   topology-S plus that group mutex. It acquires topology-X only to install a
   new fill group or repair a stale/full/reclassified fill pointer. Immediate
   young insertion follows the analogous young-fill protocol. Explicit
   synchronous make-young/make-old preflight destination capacity and lock
   source/destination groups in deterministic order. On rollover they release
   S and subordinate locks, acquire X without upgrading, install/revalidate a
   destination, and retry; a page is never detached while obtaining one.
8. Eviction and removal commit under topology-S plus the existing page-hash,
   block/zip, and source-group locks. Slot/back-pointer removal and atomic
   page-count deltas happen immediately. An emptied linked group is marked
   once and published to a bounded empty-group candidate queue while taking an
   explicit maintenance reference. It remains a valid linked empty group until
   a topology-X maintenance batch revalidates generation, emptiness,
   active-fill/boundary status, hazards, and the queue reference before
   unlinking it.
9. Empty-group candidate publication must not allocate or block while holding
   a page/group lock. Duplicate candidates are suppressed by an atomic pending
   bit. Queue ownership contributes an explicit per-group maintenance
   reference, so no queued raw pointer can be retired, destroyed, or reused
   before consumption; generation checking alone is not a lifetime mechanism.
   Queue overflow releases the attempted reference and sets a bounded
   topology-maintenance scan request with a persistent cursor so empty groups
   cannot leak indefinitely. Repopulation clears/consumes pending state only
   under the group lock; an active fill group that is later abandoned must
   republish itself if still empty. Compaction skips referenced or pending
   groups.
10. `LRU_n_pages` and `LRU_old_len` remain exact page counts using atomic
    deltas. Make `buf_page_t::old` an atomic heuristic flag so existing
    block-mutex-only readers and topology/group writers have no C++ data race.
    A page move under topology-S plus its block/source/destination protection
    stores the destination classification before publishing the destination
    slot/back-pointer; its exact old-count delta linearizes with that move.
    `group->old` changes only under topology-X plus the group lock, and
    boundary reclassification atomically publishes the same value to every
    member page. Boundary adjustment performs a fixed number of strictly
    improving group steps per activation and reschedules convergence without a
    self-wake loop.
11. Before enabling any topology-S mutation, classify every field formerly
    protected by `LRU_list_mutex` as group-local, atomic/per-thread,
    separately latched, or topology-X-only. This includes `LRU_bytes`,
    `freed_page_clock`, made-young/read-ahead statistics, compaction
    pending/epoch/retry state, and new instrumentation. Protect `unzip_LRU`
    with a dedicated ordered latch acquired after block/zip protection and
    before group mutexes; it must not force topology-X on ordinary
    uncompressed operations. An unzip scan may not hold that latch while
    waiting for block protection: snapshot bounded `(page_id,
    residency_generation)` values under the unzip latch, release it, resolve
    and temporarily fix each identity under its hash latch, then acquire block
    protection and revalidate. Never retain an unfixed raw descriptor snapshot.
12. Crossing `BUF_LRU_OLD_MIN_LEN` is an X-only structural transition.
    Atomic count adjustment uses a boundary-state/CAS protocol: an S-mode
    insertion/removal may commit only when its delta remains strictly within
    the current active/inactive regime; a delta that initializes or clears
    `LRU_old` retries under topology-X. Never expose a count/boundary mismatch.
13. Compaction acquires topology-X and ordered group mutexes, remains bounded,
    preserves flattened page order, and skips active, staging, referenced,
    boundary, or otherwise unstable groups. It must not run concurrently with
    staging publication under `LRU_drain_mutex`.
14. LRU flush and optimistic eviction traversal use topology-S plus the
    current group mutex for snapshots. When dropping topology-S for I/O or
    provisional checks, they retain only identity snapshots and registered
    atomic generation-aware hazard slots. Publish a hazard, revalidate it
    under topology-S, and clear it after resumption. Topology-X unlink adjusts
    or waits out hazards before reuse; no stale pointer may be dereferenced.
15. FILE_PAGE→ZIP_PAGE keep-zip conversion preserves page-hash X continuity
    and replaces the descriptor in its existing group slot under topology-S,
    hash X, block/zip and group protection. It preserves group, slot,
    classification, and page counts; it must not remove/reinsert through a
    fill group or acquire topology-X while holding hash X. Buddy relocation
    uses the same in-place group-slot replacement protocol.
16. A hash-visible page in a staging group remains logically in the
    replacement set but is not topology-traversable. Define explicit page/group
    staging state; mutation, flush, make-old, relocation, AHI/admin inspection,
    and non-owner validation must skip/retry it. Temporary fixes prevent
    eviction, while exhaustive validation and invalidation rendezvous through
    `LRU_drain_mutex` and require no detached staging group on return.
17. Resize, invalidation, shutdown, compressed-page conversion, buddy
    relocation, AHI/drop scans, information-schema traversal, dump, and debug
    validation must use explicit S/X topology modes and preserve existing lock
    order and lifecycle rendezvous.
18. Preserve all current queue ABA/lifecycle guarantees, old-block sysvar
    semantics, LRU manager ownership/wakeup behavior, bounded maintenance,
    compressed-page behavior, and observable LRU statistics.
19. Add low-overhead instrumentation for topology S/X acquisitions and wait
    time, group-lock acquisitions/contention, pages moved per staging
    publication, empty-group queue depth/overflow, X hold duration, boundary
    work, and fallback paths. Do not add a contended foreground global
    counter.

## Acceptance Criteria

1. Source inspection and debug assertions classify every former
   `LRU_list_mutex` site as topology-S, topology-X, group-only/private, or
   removed; no unclassified compatibility wrapper remains.
2. Two threads can mutate distinct linked groups concurrently while both hold
   topology-S; a deterministic test proves neither operation waits for the
   other group's mutex.
3. Latch-order debug runs report no inversion, and no code waits for topology
   or page/block/frame latches while holding a group mutex. Tests reject every
   recursive/cross-mode topology acquisition, upgrade, and hidden helper
   reacquisition.
4. A promotion chunk performs no topology-X acquisition per page, publishes
   at most one staging group containing at most `BUF_LRU_GROUP_SIZE` pages,
   and performs one bounded topology-X publication/reclaim/boundary phase.
5. Every promoted page is continuously represented by exactly one linked or
   staging group; no hash-visible page has a null/stale group pointer, and all
   temporary fixes are released after publication on every exit path.
6. A partial final staging group is linked at the end of the same activation.
   Close/invalidation waits for the consumer and never observes detached
   staging pages.
7. Common fresh insertion into a reusable fill group uses topology-S and one
   group mutex without topology-X. Exactly one contender installs a
   replacement group when the prior fill group is full.
8. Common eviction/removal of a page from a non-empty group uses topology-S
   and one group mutex without topology-X. Empty groups are eventually
   reclaimed despite queue collision/overflow and are never reclaimed while
   repopulated, active, referenced, or generation-mismatched. A promotion
   source skipped during publication retains ownership through the global
   queue or persistent fallback scan.
9. Atomic `LRU_n_pages` and `LRU_old_len` equal validator recomputation after
   concurrent promotion, insertion, eviction, compaction, and boundary
   movement. Atomic page-old values match group classification. Boundary
   adjustment is bounded and converges after quiescence; concurrent transitions
   at `BUF_LRU_OLD_MIN_LEN` never expose inconsistent count/boundary state.
10. A source-level lock inventory shows every formerly list-mutex-protected
    field has one explicit owner. Concurrent topology-S mutation cannot race on
    statistics, clocks, compaction state, instrumentation, or `unzip_LRU`;
    unzip scans use fixed identity snapshots and never wait for a block mutex
    while holding the unzip latch.
11. Keep-zip conversion and buddy relocation preserve hash continuity and the
    existing group/slot/count/classification without topology-X or fill-group
    reinsertion.
12. Queued empty groups and published hazards hold real lifetime ownership;
    invalidation drains them before destruction, queue overflow rolls back its
    reference, and TSAN/ASAN review finds no generation-after-free validation.
13. Staging pages are skipped/retried by non-owner paths and no staging group
    survives drain close, exhaustive validation, invalidation, or shutdown.
14. Flush/eviction scans do not dereference a reclaimed/reused group and do
    not restart quadratically from the global tail.
15. Existing queue tests, grouped-LRU validators, focused MTR tests, debug
    startup/DML/shutdown smoke, and available ASAN/TSAN checks pass.
16. External sysbench at 64/128/256/512 threads (12 GiB pool, 24 GiB data)
    shows topology-X and group-lock metrics consistent with cross-group
    concurrency. No TPS improvement is claimed before that run.

## Testing Plan

1. Add standalone/debug lock-protocol tests before behavior changes:
   topology S concurrency, X exclusion/writer progress, deterministic ordered
   group locking, S recursion, illegal upgrade/order, and legacy-site mode
   classification assertions.
2. Add group-state/lifetime tests for linked→staging publication,
   linked→empty-candidate→retired, maintenance-reference ownership,
   reuse-generation mismatch, repopulation, hazard skip, queue-overflow
   reference rollback/fallback, and invalidation rendezvous.
3. Add promotion tests for full and partial chunks, pages from distinct/same
   source groups, source groups becoming empty, stale identities, block-lock
   contention, exact old deltas, publication failure cleanup, and balanced
   temporary fixes.
4. Add insertion tests for concurrent append, fill-group rollover, stale fill
   pointer retry, old-boundary placement, exact threshold crossings, and
   compressed pages.
5. Add eviction/removal tests proving non-empty-group commits avoid topology-X
   and empty groups converge through bounded reclamation.
6. Add shared-state tests for atomic counters/page-old/statistics/epochs and
   dedicated `unzip_LRU` locking. Add keep-zip and buddy-relocation tests that
   assert in-place group-slot identity and uninterrupted page-hash ownership.
7. Extend compaction, atomic hazard registration, optimistic scan, flush
   resumption, staging visibility, validator, resize/invalidation, recovery,
   AHI, dump, information-schema, and shutdown coverage for explicit topology
   modes and group generation/reference ownership.
8. After every stage, run the smallest build/unit target first, then focused
   MTR. Run debug sync-order smoke before widening migration. Finish with
   formatting, source-lock classification, full available build/MTR,
   sanitizer review, and external sysbench handoff.

## Implementation Plan

1. Add failing lock-protocol and group-state tests/debug hooks. Introduce
   topology S/X wrapper types and per-group mutex/latch registration without
   migrating behavior; build and run the new tests.
2. Mechanically migrate every legacy `LRU_list_mutex` site to topology-X and
   restore a green build/test baseline. Add mode assertions and a complete
   source inventory; do not enable topology-S mutation yet.
3. Classify/convert all shared state: exact atomic page counters and page-old
   flag, per-thread/atomic statistics and epochs, dedicated `unzip_LRU` latch,
   and bounded X-only old-boundary state machine. Test concurrent deltas,
   threshold transitions, and quiescent convergence while mutations remain X.
4. Add explicit group states, maintenance references, atomic hazard registry,
   generation-aware empty-candidate publication, persistent overflow cursor,
   and bounded X-latched reclamation. Make invalidation drain/cancel all
   ownership. Test lifetime, dedup, overflow, repopulation, and teardown.
5. Convert keep-zip descriptor replacement and buddy relocation to in-place
   group-slot protocols while still using topology-X. Test hash continuity,
   compressed/unzip bookkeeping, and exact counters.
6. Implement one-group private promotion staging with tests first. Set chunk
   size to group size, migrate source detach to topology-S + block/group lock,
   and publish the full/partial staging group in one topology-X phase. Validate
   staging visibility, balanced fixes/references, and lifecycle close.
7. Migrate fresh old/young fill insertion and synchronous make-young/make-old
   to topology-S + ordered group locks, with no-upgrade topology-X
   rollover/retry and threshold transition fallback. Test concurrent fillers,
   cross-group moves, and placement.
8. Migrate eviction/removal commit to topology-S + existing hash/block plus
   group locks and referenced empty-candidate publication. Test the non-empty
   fast path, compressed pages, queue overflow, and every reclaim race.
9. Migrate flush and optimistic scan snapshots to topology-S plus atomic
   generation-aware hazards. Test publish/revalidate, I/O unlock windows,
   group reuse, and resumption.
10. Migrate compaction and all remaining administrative, resize,
    invalidation, validation, AHI, dump, and information-schema paths to
    explicit topology modes. Remove the old mutex and compatibility wrappers.
11. Run focused and broad validation, independent correctness/performance
    review, and prepare the external sysbench matrix and latch metrics.
