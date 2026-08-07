# LRU Group Bounded Reserve and Reclamation

## As Is

`buf_pool->LRU_group_cache` is an unbounded free-list of empty `buf_lru_group_t`
objects. Every emptied group is cached for the pool lifetime and destroyed only
by `buf_LRU_free_group_cache()` at teardown. That design was required when the
promotion drain captured raw `buf_lru_group_t *` values across
`LRU_list_mutex` release. Deferred promotion now stores
`(page_id_t, residency_generation)` only, so no asynchronous raw group capture
remains, but the unbounded cache still retains peak group memory forever.

## To Be

Empty groups enter a small reusable reserve. Overflow is moved to a retired
list under `LRU_list_mutex` and destroyed only after that mutex is released.
Invalidation and teardown free both reserve and retired groups. Each recycled
or newly allocated group receives a monotonic reuse generation used by debug
validation. Steady-state allocation/mutex creation remains off the common path
while reserve hits succeed.

## Requirements

1. Maintain a bounded reusable reserve of empty groups
   (`BUF_LRU_GROUP_RESERVE_MAX`).
2. When releasing an empty group into a full reserve, retire it instead of
   growing the reserve without bound.
3. Destroy retired groups outside `LRU_list_mutex`.
4. Invalidation and pool teardown must free every reserved and retired group.
5. Assign a monotonic per-pool reuse generation when a group is allocated
   (fresh or from reserve); debug validation must observe non-zero generation
   for linked groups.
6. Do not reintroduce asynchronous raw group-pointer captures.

## Acceptance Criteria

1. `LRU_group_cache_len` never exceeds `BUF_LRU_GROUP_RESERVE_MAX`.
2. Overflow releases increase the retired list rather than the reserve.
3. `buf_LRU_destroy_retired_groups()` takes the retired list under
   `LRU_list_mutex`, releases that mutex, then performs `mutex_free` and heap
   delete.
4. After invalidation of an empty pool, both reserve and retired lengths are
   zero.
5. Linked groups in `buf_LRU_validate_instance()` have `reuse_generation != 0`.
6. Source inspection shows no promotion/eviction path storing a raw group
   pointer beyond the locks that keep that group live.

## Testing Plan

1. Source review of alloc/release/destroy/invalidation/teardown ordering.
2. Debug validation asserts reserve bound and linked-group generations.
3. Rebuild `innobase` and `bounded_mpsc_queue-t`; run the queue unit tests.

## Implementation Plan

1. Add reserve/retired fields, reuse generation, and constants; update comments.
2. Bound `buf_lru_group_release()`, implement destroy-retired helper, and call
   it from flush/free/invalidation/teardown sites that can drop
   `LRU_list_mutex`.
3. Extend debug validation; rebuild and run the narrow tests above.
