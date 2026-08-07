# Sparse LRU Group Compaction

## As Is

Promotion, eviction, and fill-group abandonment leave partially filled groups
on `buf_pool->LRU`. Adjacent same-class groups are not merged, so the group
list grows sparser than necessary and flush/eviction walk more group nodes.

## To Be

A bounded background compaction pass merges eligible adjacent same-class
groups while preserving flattened page order (group order, then ascending
slot order within each group). Skipped groups: `LRU_old`, active fill
groups, and groups referenced by `lru_hp` / `lru_scan_itr` /
`single_scan_itr`. Repeated passes after producer quiescence leave only
non-mergeable or intentionally skipped partial groups.

## Requirements

1. Compact at most `BUF_LRU_COMPACT_BUDGET` adjacent merges per activation.
2. Merge only when both groups share `old`, neither is skipped, and
   `n_pages` sums to at most `BUF_LRU_GROUP_SIZE`.
3. Move pages from the successor into free slots of the predecessor in
   ascending successor-slot order, updating `lru_group`/`lru_slot`.
4. Reclaim the emptied successor through the existing release path.
5. Invoke compaction from the page-cleaner coordinator after promotion drain.
6. Debug validation must still reconcile counts, old flags, bitmaps, and
   reserve bounds after compaction.

## Acceptance Criteria

1. A pair of adjacent young (or old) non-skipped groups with combined occupancy
   ≤ 32 becomes one group after a compaction activation that reaches them.
2. Flattened `(group, slot)` order of surviving pages is unchanged relative to
   pre-merge concatenated order.
3. Skipped groups listed above are never emptied by compaction.
4. One activation performs ≤ `BUF_LRU_COMPACT_BUDGET` merges.
5. `innobase` builds; existing queue unit tests pass.

## Testing Plan

1. Source-review merge eligibility, skip set, and order-preserving moves.
2. Debug validate invariants remain after merges (existing validator).
3. Rebuild `innobase`; rerun `bounded_mpsc_queue-t`.

## Implementation Plan

1. Add constants and `buf_LRU_compact_sparse_groups()`.
2. Wire coordinator call; reclaim emptied groups via existing release/destroy.
3. Build/validate as above.
