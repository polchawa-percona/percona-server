# Step 3 -- classification of state formerly protected by `LRU_list_mutex`
# alone (Requirement 11), plus the exact-counter/page-old-flag conversion
# (Requirement 10). Compressed-page/`unzip_LRU` state is explicitly
# OUT OF SCOPE per user instruction: "let's ignore compressed pages (we will
# come back to them later, if the whole effort proves to make sense for
# non-compressed pages)". It is listed below only to record that it was
# seen and deliberately deferred, not converted.

## Converted to atomic (this step)

| Field | Old type | New type | Why |
|---|---|---|---|
| `buf_pool_t::LRU_n_pages` | `size_t` | `std::atomic<size_t>` | Exact page counter (Req 10); updated only via the `buf_LRU_n_pages_inc()/_dec()` choke point in buf0lru.cc so a future topology-S mover (Step 7+) has one place to add the CAS/retry logic Req 12 requires. Relaxed ordering: correct today because topology-X supplies the ordering; revisit when S-mode writers exist. |
| `buf_pool_t::LRU_old_len` | `ulint` | `std::atomic<ulint>` | Exact page counter (Req 10), same rationale; delta sites go through `buf_LRU_old_len_inc()/_dec()/_add()`. Bulk recomputation sites (`buf_LRU_old_adjust_len`, `buf_LRU_old_init`, the two invalidate/reset sites) keep plain `=` assignment -- they are whole-value overwrites, not deltas. |
| `buf_page_t::old` | `bool` | `copyable_atomic_t<bool>` (mirrors `buf_fix_count_atomic_t`) | Req 10: "atomic heuristic flag so existing block-mutex-only readers and topology/group writers have no C++ data race." Written via `buf_page_set_old()` and the two group-old-flag-flip loops (`buf_LRU_old_init`, `buf_LRU_remove_block_finish`'s LRU-too-short branch), always with `.store(..., memory_order_relaxed)` -- the topology-X + group-mutex hold is still what makes the write observable-in-order; the atomic type only removes the data race for readers that hold just a block mutex. |
| `buf_pool_stat_t::n_ra_pages_evicted` | `uint64_t` | `std::atomic<uint64_t>` | Matches sibling fields (`n_pages_read` etc.) already atomic in the same struct; no code reads its address. |
| `buf_pool_stat_t::n_pages_made_young` | `uint64_t` | `std::atomic<uint64_t>` | Same as above. |
| `buf_pool_stat_t::LRU_bytes` | `uint64_t` | `std::atomic<uint64_t>` | Same as above. |
| `buf_pool_t::freed_page_clock` | `ulint` | `std::atomic<ulint>` | Heuristic sequence number read without any latch in several `buf_page_peek_if_*` helpers; make every reader see a value consistent with *some* writer rather than a torn read. |

`buf_pool_stat_t::copy()` updated to `.store(src.load())` for the three
newly-atomic fields, matching the pattern already used for the struct's
pre-existing atomic fields. `reset()` needed no change: `= 0` already
compiles against `std::atomic` via its assignment operator, exactly as it
already did for the pre-existing atomic fields in the same function.

## Classified as topology-X-only -- no change (this step)

| Field | Why it stays X-only |
|---|---|
| `LRU_compaction_pending` (bool) | Req 13: "Compaction acquires topology-X." Every reader/writer already holds topology-X; no S-mode consumer is planned for this flag (it exists to hand a fragmentation signal from detach sites, which are also X-only through Step 8, to the compactor). |
| `LRU_compaction_retry` (bool) | Same as above. |
| `LRU_compaction_epoch` (uint64_t) | Same as above; incremented only at detach sites, which stay X-only through Step 8's plan (Req 8 talks about atomic *page-count* deltas, not the compaction epoch). |
| `LRU_compaction_sweep_epoch` (uint64_t) | Same as above; compared against `LRU_compaction_epoch` only inside the X-held compactor. |
| `LRU_young_fill_group` / `LRU_fill_group` (pointers) | Fill-group pointer replacement is explicitly still topology-X in this step (Req 7's topology-S fast path for fill insertion is Step 7, not this one). |
| `LRU_group_cache*` / `LRU_group_retired*` (group free-list bookkeeping) | Untouched this step; group lifecycle states are Step 4 (Req 3). |
| `LRU_old` (pointer) | The boundary pointer itself stays X-only; Req 12's CAS/retry protocol governs *when* a delta is allowed to commit without X, not the pointer's own protection, and is deliberately not built yet (see below). |

## Deferred by explicit user instruction (compressed pages / `unzip_LRU`)

| Field | Note |
|---|---|
| `buf_pool_t::unzip_LRU` (list base node) | Req 11 calls for "a dedicated ordered latch acquired after block/zip protection and before group mutexes." Not implemented. Currently still fully covered by topology-X (unchanged behavior), which is correct as long as no S-mode path exists yet. Must be revisited before any S-mode work touches compressed pages. |
| Buddy relocation / keep-zip conversion (Req 15) | Untouched; Step 5 in the Implementation Plan, itself gated on the same user deferral. |

## Explicitly not built this step

- **Requirement 12's `BUF_LRU_OLD_MIN_LEN` boundary CAS/retry protocol.** Per
  advisor review: a CAS with no concurrent topology-S deltas is a
  load-modify-store with extra steps, and it is untestable before Step 7
  introduces the first S-mode mutator. Building it now would mean guessing
  its shape without a caller to validate it against. The choke-point
  helpers added this step (`buf_LRU_n_pages_inc/dec`) are the correct amount
  of preparation: Step 7 modifies those helpers' bodies instead of
  re-auditing every call site.

## Known limitation carried forward (not a regression)

`i_s.cc`'s `i_s_innodb_fill_buffer_lru()` reads `LRU_n_pages` into `lru_len`,
walks the list, and re-asserts against a fresh read at the end
(`ut_ad(lru_pos == buf_pool->LRU_n_pages)`). This is correct today because
the whole scan holds topology-X throughout. It will break once Step 9/10
drops this scan to topology-S (the counter can legitimately change mid-scan
once S-mode movers exist). Not fixed now -- noted so Step 9/10 doesn't
mistake the existing assert for proof the S-mode migration is safe here.

## Validation performed for this step

- `ninja mysqld`: clean build.
- `buf0lru_topology-t` gunit: 6/6 pass.
- Manual smoke test: init, start (`--innodb-sync-debug`), ~50k rows via
  repeated batch inserts (crosses `BUF_LRU_OLD_MIN_LEN` many times over),
  repeated point-select bursts (make-young), a bulk delete, `SHOW ENGINE
  INNODB STATUS` sanity check (`Old database pages`, `Pages made young`,
  `LRU len` all consistent), clean shutdown -- no assertions, no crash
  markers in the error log.
- Full 5-test MTR suite (same set as Steps 1-2): sys_vars
  `innodb_lru_make_young_drain_threshold_basic`,
  `innodb_lru_threads_basic`; `innodb_zip/lru_mutex_narrow_stress_debug`;
  `percona_innodb/lru_flusher_debug`; `innodb/innodb_lru_threads_kill_recovery`.
  See `.requirements/20260807T154100Z_lru_two_level_locking/` sibling
  session notes / commit message for the pass/fail detail.
