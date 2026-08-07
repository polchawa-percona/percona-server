# Identity-Based Optimistic Tail Scanning

## As Is

`buf_LRU_free_from_common_LRU_list()` holds `LRU_list_mutex` for the whole
tail walk. For each page it takes the block mutex under that list mutex and
calls `buf_LRU_free_page()` / `buf_page_free_stale()`. Provisional eligibility
work therefore serializes behind the global list lock, which is the remaining
high-contention eviction path after Stages 1–9.

## To Be

Tail eviction snapshots at most one group's `(page_id, residency_generation)`
candidates under a short `LRU_list_mutex` + group-mutex section, releases the
list mutex, then provisionally inspects candidates without that mutex.
Successful eviction reacquires through the conventional
page-hash → block → LRU-list → group order, revalidates generation, hash
identity, group membership, cleanliness, fix count, and I/O state, and uses
the existing `buf_LRU_free_page()` / stale-free commit paths. No live hashed
page is group-detached before that commit.

## Requirements

1. Snapshot at most one group's non-null identities per provisional batch.
2. Release `LRU_list_mutex` before provisional eligibility inspection.
3. Commit eviction only via existing free paths after full revalidation.
4. Preserve scan hazard-pointer advancement and scanned-page accounting
   semantics relative to `BUF_LRU_SEARCH_SCAN_THRESHOLD` / `scan_all`.
5. Never perform a group-only detach of a hash-visible live page.
6. On successful free, `LRU_list_mutex` ownership matches today's contract
   (released by the free path); on failure the caller still owns it when the
   overall scan returns false.

## Acceptance Criteria

1. Source inspection shows the list mutex is not held across the provisional
   eligibility loop for a snapshotted group.
2. Eviction commit re-checks generation and rejects stale/ABA identities.
3. No new group-detach path outside `buf_LRU_free_page` /
   `buf_LRU_block_remove_hashed` / existing stale free.
4. `innobase` builds; queue unit tests still pass.

## Testing Plan

1. Source-review lock order and revalidation for the new helper.
2. Rebuild `innobase`; run `bounded_mpsc_queue-t`.

## Implementation Plan

1. Add identity snapshot + try-evict helpers.
2. Rewrite `buf_LRU_free_from_common_LRU_list()` to drop the list mutex for
   provisional work and re-enter for the next group.
3. Build/validate as above.
