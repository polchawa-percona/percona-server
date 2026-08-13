/*****************************************************************************

Copyright (c) 1995, 2026, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file buf/buf0lru.cc
 The database buffer replacement algorithm

 Created 11/5/1995 Heikki Tuuri
 *******************************************************/

#include "buf0lru.h"

#include <bit>
#include <limits>
#include <optional>

#include "btr0btr.h"
#include "btr0sea.h"
#include "buf0buddy.h"
#include "buf0buf.h"
#include "buf0dblwr.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "buf0stats.h"
#include "fil0fil.h"
#include "hash0hash.h"
#include "ibuf0ibuf.h"
#include "log0recv.h"
#include "log0write.h"
#include "my_dbug.h"
#include "os0event.h"
#include "os0file.h"
#include "page0zip.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "sync0rw.h"
#include "trx0trx.h"
#include "ut0bounded_mpsc.h"
#include "ut0byte.h"
#include "ut0rnd.h"

/** The number of blocks from the LRU_old pointer onward, including
the block pointed to, must be buf_pool->LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV
of the whole LRU list length, except that the tolerance defined below
is allowed. Note that the tolerance must be small enough such that for
even the BUF_LRU_OLD_MIN_LEN long LRU list, the LRU_old pointer is not
allowed to point to either end of the LRU list. */

constexpr uint32_t BUF_LRU_OLD_TOLERANCE = 20;

/** The minimum amount of non-old blocks when the LRU_old list exists
(that is, when there are more than BUF_LRU_OLD_MIN_LEN blocks).
@see buf_LRU_old_adjust_len */
constexpr uint32_t BUF_LRU_NON_OLD_MIN_LEN = 5;
static_assert(BUF_LRU_NON_OLD_MIN_LEN < BUF_LRU_OLD_MIN_LEN,
              "BUF_LRU_NON_OLD_MIN_LEN >= BUF_LRU_OLD_MIN_LEN");
/** When dropping the search hash index entries before deleting an ibd
file, we build a local array of pages belonging to that tablespace
in the buffer pool. Following is the size of that array.
We also release buf_pool->LRU_topology_latch after scanning this many pages of
the flush_list when dropping a table. This is to ensure that other threads are
not blocked for extended period of time when using very large buffer pools. */
static const ulint BUF_LRU_DROP_SEARCH_SIZE = 1024;

/** We scan these many blocks when looking for a clean page to evict
during LRU eviction. */
static const ulint BUF_LRU_SEARCH_SCAN_THRESHOLD = 100;

static_assert(BUF_LRU_GROUP_RESERVE_TARGET >= BUF_LRU_PROMOTE_DRAIN_CHUNK);
static_assert(BUF_LRU_GROUP_RESERVE_MAX > BUF_LRU_GROUP_RESERVE_TARGET);
static_assert(BUF_LRU_PROMOTE_DRAIN_CHUNK == BUF_LRU_GROUP_SIZE,
              "PS-11141 Requirement 5: one drained chunk must fill exactly "
              "one private staging group");

/** If we switch on the InnoDB monitor because there are too few available
frames in the buffer pool, we set this to true */
static std::atomic_bool buf_lru_switched_on_innodb_mon = false;

/** These statistics are not 'of' LRU but 'for' LRU.  We keep count of I/O
 and page_zip_decompress() operations.  Based on the statistics,
 buf_LRU_evict_from_unzip_LRU() decides if we want to evict from
 unzip_LRU or the regular LRU.  From unzip_LRU, we will only evict the
 uncompressed frame (meaning we can evict dirty blocks as well).  From
 the regular LRU, we will evict the entire block (i.e.: both the
 uncompressed and compressed data), which must be clean. */

/** @{ */

/** Number of intervals for which we keep the history of these stats.
Each interval is 1 second, defined by the rate at which
srv_error_monitor_thread() calls buf_LRU_stat_update(). */
static const ulint BUF_LRU_STAT_N_INTERVAL = 50;

/** Co-efficient with which we multiply I/O operations to equate them
with page_zip_decompress() operations. */
static const ulint BUF_LRU_IO_TO_UNZIP_FACTOR = 50;

/** Sampled values buf_LRU_stat_cur.
Not protected by any mutex.  Updated by buf_LRU_stat_update(). */
static buf_LRU_stat_t buf_LRU_stat_arr[BUF_LRU_STAT_N_INTERVAL];

/** Cursor to buf_LRU_stat_arr[] that is updated in a round-robin fashion. */
static ulint buf_LRU_stat_arr_ind;

/** Current operation counters.  Not protected by any mutex.  Cleared
by buf_LRU_stat_update(). */
buf_LRU_stat_t buf_LRU_stat_cur;

/** Running sum of past values of buf_LRU_stat_cur.
Updated by buf_LRU_stat_update().  Not Protected by any mutex. */
buf_LRU_stat_t buf_LRU_stat_sum;

/** @} */

/** @name Heuristics for detecting index scan
@{ */
/** Move blocks to "new" LRU list only if the first access was at
least this many milliseconds ago.  Not protected by any mutex or latch. */
uint buf_LRU_old_threshold;
std::chrono::milliseconds get_buf_LRU_old_threshold() {
  return std::chrono::milliseconds{buf_LRU_old_threshold};
}

uint buf_LRU_make_young_drain_threshold;

void buf_LRU_enqueue_promote(buf_page_t *bpage) {
  ut_ad(bpage->buf_fix_count > 0);

  auto *const buf_pool = buf_pool_from_bpage(bpage);
  if (!buf_pool->LRU_accept_promotions.load(std::memory_order_acquire)) {
    return;
  }

  const auto identity =
      buf_lru_promote_t{bpage->id, bpage->residency_generation};
  if (!buf_pool->LRU_promote_dedup->try_reserve(
          identity.residency_generation)) {
    return;
  }

  auto *const queue = buf_pool->LRU_promote_queue;
  const uint32_t wake_threshold =
      std::min<uint32_t>(buf_LRU_make_young_drain_threshold,
                         queue->capacity());

  /* Promotion is heuristic. A busy selected slot is dropped immediately;
  producers never pin the descriptor, allocate, take an InnoDB mutex, or
  drain foreground work. */
  const auto result = queue->try_push(identity, wake_threshold);
  if (result == ut::Bounded_mpsc_push_result::full) {
    static_cast<void>(
        buf_pool->LRU_promote_dedup->release(identity.residency_generation));
    return;
  }

  if (result == ut::Bounded_mpsc_push_result::first) {
    os_event_set(buf_flush_event);
  }
}

/** @} */

/** Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed.

The caller must hold buf_pool->LRU_topology_latch, the buf_page_get_mutex()
mutex and the appropriate hash_lock. This function will release the
buf_page_get_mutex() and the hash_lock.

If a compressed page is freed other compressed pages may be relocated.

@param[in]      bpage           block, must contain a file page and
                                be in a state where it can be freed; there
                                may or may not be a hash index to the page
@param[in]      zip             true if should remove also the
                                compressed page of an uncompressed page
@param[in]      ignore_content  true if should ignore page content, since it
                                could be not initialized
@param[in]      keep_hash_lock  true if the hash cell X-latch should be kept
                                by this function instead of being released;
                                only allowed when the caller will re-insert
                                a compressed-only descriptor for this page
                                id into the page hash (the keep-zip path of
                                buf_LRU_free_page()), so that the page id is
                                never observably absent from the page hash
@retval true if BUF_BLOCK_FILE_PAGE was removed from page_hash. The
caller needs to free the page to the free list
@retval false if BUF_BLOCK_ZIP_PAGE was removed from page_hash. In
this case the block is already returned to the buddy allocator. */
[[nodiscard]] static bool buf_LRU_block_remove_hashed(
    buf_page_t *bpage, bool zip, bool ignore_content,
    bool keep_hash_lock = false);

/** Puts a file page whose has no hash index to the free list.
@param[in,out] block            Must contain a file page and be in a state
                                where it can be freed. */
static void buf_LRU_block_free_hashed_page(buf_block_t *block) noexcept;

/** Increases LRU size in bytes with page size inline function
@param[in]      bpage           control block
@param[in]      buf_pool        buffer pool instance */
static inline void incr_LRU_size_in_bytes(buf_page_t *bpage,
                                          buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());

  buf_pool->stat.LRU_bytes += bpage->size.physical();

  ut_ad(buf_pool->stat.LRU_bytes <= buf_pool->curr_pool_size);
}

/** Determines if the unzip_LRU list should be used for evicting a victim
instead of the general LRU list.
@param[in,out]  buf_pool        buffer pool instance
@return true if should use unzip_LRU */
bool buf_LRU_evict_from_unzip_LRU(buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  /* If the unzip_LRU list is empty, we can only use the LRU. */
  if (UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0) {
    return false;
  }

  /* If unzip_LRU is at most 10% of the size of the LRU list,
  then use the LRU.  This slack allows us to keep hot
  decompressed pages in the buffer pool. */
  if (UT_LIST_GET_LEN(buf_pool->unzip_LRU) <= buf_pool->LRU_n_pages / 10) {
    return false;
  }

  /* If eviction hasn't started yet, we assume by default
  that a workload is disk bound. */
  if (buf_pool->freed_page_clock == 0) {
    return true;
  }

  /* Calculate the average over past intervals, and add the values
  of the current interval. */
  ulint io_avg =
      buf_LRU_stat_sum.io / BUF_LRU_STAT_N_INTERVAL + buf_LRU_stat_cur.io;

  ulint unzip_avg =
      buf_LRU_stat_sum.unzip / BUF_LRU_STAT_N_INTERVAL + buf_LRU_stat_cur.unzip;

  /* Decide based on our formula.  If the load is I/O bound
  (unzip_avg is smaller than the weighted io_avg), evict an
  uncompressed frame from unzip_LRU.  Otherwise we assume that
  the load is CPU bound and evict from the regular LRU. */
  return (unzip_avg <= io_avg * BUF_LRU_IO_TO_UNZIP_FACTOR);
}

/** Attempts to drop page hash index on a batch of pages belonging to a
particular space id.
@param[in]      space_id        space id
@param[in]      page_size       page size
@param[in]      arr             array of page_no
@param[in]      count           number of entries in array */
static void buf_LRU_drop_page_hash_batch(space_id_t space_id,
                                         const page_size_t &page_size,
                                         const page_no_t *arr, ulint count) {
  ut_ad(count <= BUF_LRU_DROP_SEARCH_SIZE);

  for (ulint i = 0; i < count; ++i, ++arr) {
    /* While our only caller
    buf_LRU_drop_page_hash_for_tablespace()
    is being executed for DROP TABLE or similar,
    the table cannot be evicted from the buffer pool.
    Note: this should not be executed for DROP TABLESPACE,
    because DROP TABLESPACE would be refused if tables existed
    in the tablespace, and a previous DROP TABLE would have
    already removed the AHI entries. */
    btr_search_drop_page_hash_when_freed(page_id_t(space_id, *arr), page_size);
  }
}

/** When doing a DROP TABLE/DISCARD TABLESPACE we have to drop all page
hash index entries belonging to that table. This function tries to
do that in batch. Note that this is a 'best effort' attempt and does
not guarantee that ALL hash entries will be removed.
@param[in]      buf_pool        buffer pool instance
@param[in]      space_id        space id */
static void buf_LRU_drop_page_hash_for_tablespace(buf_pool_t *buf_pool,
                                                  space_id_t space_id) {
  bool found;
  const page_size_t page_size(fil_space_get_page_size(space_id, &found));

  if (!found) {
    /* Somehow, the tablespace does not exist.  Nothing to drop. */
    ut_d(ut_error);
    ut_o(return);
  }

  page_no_t *page_arr = static_cast<page_no_t *>(ut::malloc_withkey(
      UT_NEW_THIS_FILE_PSI_KEY, sizeof(page_no_t) * BUF_LRU_DROP_SEARCH_SIZE));

  ulint num_entries = 0;

  /* This administrative best-effort scan can afford to wait out an
  in-flight promotion/compaction activation across its unlocked batches. */
  mutex_enter(&buf_pool->LRU_drain_mutex);

  buf_pool->LRU_topology_latch.x_lock();

scan_again:
  /* PS-11141 grouped LRU list: walk groups tail-to-head, then each
  group's pages. Exact resumption position after an array-full mutex
  release no longer matters (and would be unsafe to reconstruct from a
  single page pointer across groups): this is an explicitly best-effort
  scan (see the function doc comment), so we simply restart the whole
  scan from the tail after each release -- strictly safer than trying to
  resume, at the modest, rare cost of re-examining already-processed
  pages (harmless: dropping an already-dropped hash entry is a no-op). */
  for (buf_lru_group_t *group = UT_LIST_GET_LAST(buf_pool->LRU);
       group != nullptr; group = UT_LIST_GET_PREV(LRU, group)) {
    for (auto *bpage : group->pages) {
      if (bpage == nullptr) {
        continue;
      }

      ut_a(buf_page_in_file(bpage));

      if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE ||
          bpage->id.space() != space_id || bpage->was_io_fixed()) {
        /* Compressed pages are never hashed.
        Skip blocks of other tablespaces.
        Skip I/O-fixed blocks (to be dealt with later). */
        continue;
      }

      buf_block_t *block = reinterpret_cast<buf_block_t *>(bpage);

      mutex_enter(&block->mutex);

      block->ahi.validate();

      bool skip = bpage->buf_fix_count > 0 || !block->ahi.index;

      mutex_exit(&block->mutex);

      if (skip) {
        /* Skip this block, because there are
        no adaptive hash index entries
        pointing to it, or because we cannot
        drop them due to the buffer-fix. */
        continue;
      }

      /* Store the page number so that we can drop the hash
      index in a batch later. */
      page_arr[num_entries] = bpage->id.page_no();
      ut_a(num_entries < BUF_LRU_DROP_SEARCH_SIZE);
      ++num_entries;

      if (num_entries < BUF_LRU_DROP_SEARCH_SIZE) {
        continue;
      }

      /* Array full. We release the topology latch to obey
      the latching order. */
      buf_pool->LRU_topology_latch.x_unlock();

      buf_LRU_drop_page_hash_batch(space_id, page_size, page_arr, num_entries);

      num_entries = 0;

      buf_pool->LRU_topology_latch.x_lock();

      goto scan_again;
    }
  }

  buf_pool->LRU_topology_latch.x_unlock();
  mutex_exit(&buf_pool->LRU_drain_mutex);

  /* Drop any remaining batch of search hashed pages. */
  buf_LRU_drop_page_hash_batch(space_id, page_size, page_arr, num_entries);
  ut::free(page_arr);
}

/** Try to pin the block in buffer pool. Once pinned, the block cannot be moved
within flush list or removed. The dirty page can be flushed when we release the
flush list mutex. We return without pinning in that case.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  bpage     page to remove
@return true if page could be pinned successfully. */
static bool buf_page_try_pin(buf_pool_t *buf_pool, buf_page_t *bpage) {
  /* Allow pin/unpin with NULL. */
  if (bpage == nullptr) {
    return true;
  }

  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(buf_flush_list_mutex_own(buf_pool));
  ut_ad(bpage->in_flush_list);

  /* To take care of the ABA problem that the block get flushed and
  re-inserted into flush list, we can check the oldest LSN. It is
  safe to access oldest LSN with flush list mutex protection as it
  is set and reset while adding and removing from flush list. */
  auto saved_oldest_lsn = bpage->get_oldest_lsn();

  buf_flush_list_mutex_exit(buf_pool);

  /* The topology latch ensures that the page descriptor cannot be freed
  for both compressed and uncompressed page. */
  BPageMutex *block_mutex = buf_page_get_mutex(bpage);
  mutex_enter(block_mutex);

  bool pinned = false;

  /* Recheck the I/O fix and the flush list presence now that we
  hold the right mutex */
  if (buf_page_get_io_fix(bpage) == BUF_IO_NONE && bpage->is_dirty() &&
      saved_oldest_lsn == bpage->get_oldest_lsn()) {
    /* "Fix" the block so that the position cannot be
    changed after we release the buffer pool and
    block mutexes. */
    buf_page_set_sticky(bpage);
    pinned = true;
    ut_ad(bpage->in_flush_list);
  }

  mutex_exit(block_mutex);
  buf_flush_list_mutex_enter(buf_pool);

  return pinned;
}

/** Unpin the block in buffer pool. Ensure that the dirty page cannot be
flushed even though we need to release the flush list mutex momentarily.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  bpage     page to remove */
static void buf_page_unpin(buf_pool_t *buf_pool, buf_page_t *bpage) {
  /* Allow pin/unpin with NULL. */
  if (bpage == nullptr) {
    return;
  }

  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(buf_flush_list_mutex_own(buf_pool));

  buf_flush_list_mutex_exit(buf_pool);

  BPageMutex *block_mutex = buf_page_get_mutex(bpage);
  mutex_enter(block_mutex);

  /* "Unfix" the block now that we have both the LRU list and block mutexes . */
  buf_page_unset_sticky(bpage);

  buf_flush_list_mutex_enter(buf_pool);

  /* Release block mutex only after re-acquiring the flush list mutex to
  avoid any window where the page could have been flushed concurrently. The
  block mutex must be acquired before flushing a page. */
  mutex_exit(block_mutex);
}

/** If we have hogged the resources for too long then release the LRU list and
flush list mutexes and do a thread yield. Set the current page to "sticky" so
that it is not relocated during the yield. If I/O is started before sticky BIT
could be set, we skip yielding. The caller should restart the scan.
@param[in,out]  buf_pool        buffer pool instance
@param[in,out]  bpage           page to remove
@param[in]      processed       number of pages processed
@param[out]     restart         if caller needs to restart scan
@return true if yielded. */
[[nodiscard]] static bool buf_flush_try_yield(buf_pool_t *buf_pool,
                                              buf_page_t *bpage,
                                              size_t processed, bool &restart) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(buf_flush_list_mutex_own(buf_pool));

  restart = false;

  /* Every BUF_LRU_DROP_SEARCH_SIZE iterations in the loop we release
  buf_pool->LRU_topology_latch to let other threads do their job but only if the
  block is not IO fixed. This ensures that the block stays in its position in
  the flush_list. We read io_fix without block_mutex, because we will recheck
  with block_mutex.*/
  if (bpage != nullptr && processed >= BUF_LRU_DROP_SEARCH_SIZE &&
      bpage->was_io_fix_none()) {
    if (!buf_page_try_pin(buf_pool, bpage)) {
      restart = true;
      return false;
    }

    ut_ad(bpage->in_flush_list);
    ut_d(auto oldest_lsn = bpage->get_oldest_lsn());

    /* Now it is safe to release the topology latch. */
    buf_flush_list_mutex_exit(buf_pool);
    buf_pool->LRU_topology_latch.x_unlock();

    /* Try and force a context switch. */
    std::this_thread::yield();

    buf_pool->LRU_topology_latch.x_lock();
    buf_flush_list_mutex_enter(buf_pool);

    buf_page_unpin(buf_pool, bpage);

    /* Should not have been removed from the flush list during the yield. */
    ut_ad(bpage->in_flush_list);

    /* The oldest LSN change would mean the page is removed and inserted back.
    This ABA issue is handled during pinning and should not be the case. */
    ut_ad(oldest_lsn == bpage->get_oldest_lsn());

    return true;
  }
  return false;
}

/** Check if a dirty page should be flushed or removed based on space ID and
flush observer.
@param[in]  page        dirty page in flush list
@param[in]  observer    Flush observer
@param[in]  space       Space ID
@return true, if page should considered for flush or removal. */
static inline bool check_page_flush_observer(buf_page_t *page,
                                             const Flush_observer *observer,
                                             space_id_t space) {
  /* If no flush observer then compare space ID. */
  if (observer == nullptr) {
    return (space == page->id.space());
  }
  /* Otherwise, match the flush observer pointer. */
  return (observer == page->get_flush_observer());
}

/** Attempts to remove a single page from flush list. It is fine to
skip flush if the page flush is already in progress.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  bpage     page to remove
@return true if page could be removed successfully. */
static bool remove_page_flush_list(buf_pool_t *buf_pool, buf_page_t *bpage) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(buf_flush_list_mutex_own(buf_pool));

  /* It is safe to check bpage->space and bpage->io_fix while holding
  buf_pool->LRU_topology_latch only. We will repeat the check of io_fix
  under block_mutex later, this is just an optimization to avoid the
  mutex acquisition if its likely io_fix is not NONE. */
  if (bpage->was_io_fixed()) {
    /* We cannot remove this page during this scan. */
    return false;
  }
  BPageMutex *block_mutex = buf_page_get_mutex(bpage);

  /* We don't have to worry about bpage becoming a dangling pointer by a
  compressed page flush list relocation because we hold LRU mutex. */
  buf_flush_list_mutex_exit(buf_pool);
  mutex_enter(block_mutex);

  bool removed = false;
  /* Recheck the page I/O fix and the flush list presence now that we hold
  the right mutex. */
  if (buf_page_get_io_fix(bpage) == BUF_IO_NONE && bpage->is_dirty()) {
    buf_flush_remove(bpage);
    removed = true;
  }

  mutex_exit(block_mutex);
  buf_flush_list_mutex_enter(buf_pool);

  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  return removed;
}

/** Remove all dirty pages belonging to a given tablespace inside a specific
buffer pool instance. The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU. We don't check for interrupt
as we must finish the operation. Usually this function is called as a cleanup
work after an interrupt is received.
@param[in,out]  buf_pool  buffer pool instance
@param[in]      id        space id for which to remove or flush pages
@param[in]      observer  flush observer to identify specific pages
@retval DB_SUCCESS if all freed
@retval DB_FAIL if not all freed and caller should call function again. */
[[nodiscard]] static dberr_t remove_pages_flush_list(buf_pool_t *buf_pool,
                                                     space_id_t id,
                                                     Flush_observer *observer) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  buf_flush_list_mutex_enter(buf_pool);

  buf_page_t *prev = nullptr;
  size_t processed = 0;
  dberr_t error = DB_SUCCESS;

  for (buf_page_t *bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
       bpage != nullptr; bpage = prev) {
    ut_a(buf_page_in_file(bpage));

    /* Save the previous page before freeing the current one. */
    prev = UT_LIST_GET_PREV(list, bpage);

    if (check_page_flush_observer(bpage, observer, id)) {
      /* Try to PIN the previous page before removing. This would let us
      continue the current iteration after removal. */
      bool pinned = buf_page_try_pin(buf_pool, prev);

      /* Try to remove the current page even if pin failed. */
      bool removed = remove_page_flush_list(buf_pool, bpage);

      if (!pinned) {
        /* We should not trust prev pointer as PIN was unsuccessful. */
        error = DB_FAIL;
        break;
      }

      if (!removed) {
        /* Currently we come back and re-check. The iteration can continue.
        In future, it is also possible to wait for the concurrent IO to complete
        to avoid re-scanning. */
        error = DB_FAIL;
      }

      buf_page_unpin(buf_pool, prev);
    }

    ++processed;

    /* Yield if we have hogged the CPU and mutexes for too long. */
    bool restart = false;
    if (buf_flush_try_yield(buf_pool, prev, processed, restart)) {
      ut_ad(!restart);
      /* Reset the batch size counter if we had to yield. */
      processed = 0;
    }

    if (restart) {
      /* The previous page is already flushed or being flushed. We need at least
      another iteration. Current iteration can continue. */
      error = DB_FAIL;
    }
  }

  buf_flush_list_mutex_exit(buf_pool);
  return error;
}

/** Flushes a single page inside a buffer pool instance.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  bpage     page to flush
@return true if page was flushed. */
static bool flush_page_flush_list(buf_pool_t *buf_pool, buf_page_t *bpage) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(buf_flush_list_mutex_own(buf_pool));

  if (bpage->was_io_fixed()) {
    return false;
  }

  BPageMutex *block_mutex = buf_page_get_mutex(bpage);

  /* We don't have to worry about bpage becoming a dangling pointer by a
  compressed page flush list relocation because we hold LRU mutex. */
  buf_flush_list_mutex_exit(buf_pool);

  mutex_enter(block_mutex);
  bool flushed = false;

  if (buf_flush_ready_for_flush(bpage, BUF_FLUSH_SINGLE_PAGE)) {
    /* We trigger single page flush and async IO. However, if double write is
    used, dblwr::write() forces all single page flush to sync IO.
    1. It makes the function behaviour change from sync to async for temp
       tablespaces and if redo is disabled. The caller must not assume
       the page is flushed when we return flushed = T.
    2. For bulk flush async trigger could be better for performance and seems
       to be the case in 5.7. Need to validate if 8.0 forcing sync flush
       is intentional - No functional impact. */
    flushed = buf_flush_page(buf_pool, bpage, BUF_FLUSH_SINGLE_PAGE, false);
  }

  if (flushed) {
    /* During flush, we have already released the LRU list and block mutexes.
    Wake up possible simulated aio thread to actually post the writes to the
    operating system */
    os_aio_simulated_wake_handler_threads();
    buf_pool->LRU_topology_latch.x_lock();
  } else {
    mutex_exit(block_mutex);
  }

  buf_flush_list_mutex_enter(buf_pool);

  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  return flushed;
}

/** Remove all dirty pages belonging to a given tablespace inside a specific
buffer pool instance when we are deleting the data file(s) of that
tablespace. The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param[in,out]  buf_pool  buffer pool instance
@param[in]      id        space id for which to remove or flush pages
@param[in]      observer  flush observer
@param[in]      trx       transaction to check if the operation must be
                          interrupted, can be NULL
@retval DB_SUCCESS if all freed
@retval DB_FAIL if not all freed
@retval DB_INTERRUPTED if the transaction was interrupted */
[[nodiscard]] static dberr_t flush_pages_flush_list(buf_pool_t *buf_pool,
                                                    space_id_t id,
                                                    Flush_observer *observer,
                                                    const trx_t *trx) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  buf_flush_list_mutex_enter(buf_pool);

  buf_page_t *prev = nullptr;
  size_t processed = 0;

  dberr_t error = DB_SUCCESS;

  for (buf_page_t *bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
       bpage != nullptr; bpage = prev) {
    ut_a(buf_page_in_file(bpage));

    /* Save the previous page before flushing the current one. */
    prev = UT_LIST_GET_PREV(list, bpage);

    if (check_page_flush_observer(bpage, observer, id)) {
      /* Try to PIN the previous page before flushing. This would let us
      continue the current iteration after flush. */
      bool pinned = buf_page_try_pin(buf_pool, prev);

      /* Try to flush the current page even if pin failed. */
      flush_page_flush_list(buf_pool, bpage);

      /* Currently we come back and re-check once flush is triggered. If the
      flush is unsuccessful we need to rescan too. So, we set the error for
      rescan unconditionally here. The iteration can continue if PIN was
      successful. */
      error = DB_FAIL;

      if (!pinned) {
        /* We should not trust prev pointer as PIN was unsuccessful. */
        break;
      }

      buf_page_unpin(buf_pool, prev);
    }

    ++processed;

    /* Yield if we have hogged the CPU and mutexes for too long. */
    bool restart = false;
    if (buf_flush_try_yield(buf_pool, prev, processed, restart)) {
      ut_ad(!restart);
      /* Reset the batch size counter if we had to yield. */
      processed = 0;
    }

    if (restart) {
      /* The previous page is already flushed or being flushed. We need at least
      another iteration. Current iteration can continue. */
      error = DB_FAIL;
    }

    /* The check for trx is interrupted is expensive, we want to check every
    N iterations. */
    if (processed == 0 && trx && trx_is_interrupted(trx)) {
      if (trx->flush_observer != nullptr) {
        trx->flush_observer->interrupted();
      }
      error = DB_INTERRUPTED;
      break;
    }
  }

  buf_flush_list_mutex_exit(buf_pool);
  return error;
}

/** Remove or flush all the dirty pages that belong to a given tablespace
inside a specific buffer pool instance. The pages will remain in the LRU
list and will be evicted from the LRU list as they age and move towards
the tail of the LRU list.
@param[in,out]  buf_pool        buffer pool instance
@param[in]      id              space id
@param[in]      observer        flush observer
@param[in]      flush           flush to disk if true, otherwise remove
                                the pages without flushing
@param[in]      trx             transaction to check if the operation
                                must be interrupted
@param[in]      strict          true, if no page from tablespace
                                can be in buffer pool just after flush */
static void buf_flush_dirty_pages(buf_pool_t *buf_pool, space_id_t id,
                                  Flush_observer *observer, bool flush,
                                  const trx_t *trx, bool strict) {
  dberr_t err;

  do {
    /* TODO: it should be possible to avoid locking the LRU list
    mutex here. */
    buf_pool->LRU_topology_latch.x_lock();

    if (flush) {
      err = flush_pages_flush_list(buf_pool, id, observer, trx);

    } else {
      err = remove_pages_flush_list(buf_pool, id, observer);
    }

    buf_pool->LRU_topology_latch.x_unlock();

    ut_ad(buf_flush_validate(buf_pool));

    if (err == DB_FAIL) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (err == DB_INTERRUPTED && observer != nullptr) {
      ut_a(flush);

      flush = false;
      err = DB_FAIL;
    }

    /* DB_FAIL is a soft error, it means that the task wasn't
    completed, needs to be retried. */

    ut_ad(buf_flush_validate(buf_pool));

  } while (err == DB_FAIL);

  ut_ad(observer != nullptr || err == DB_INTERRUPTED || !strict ||
        buf_pool_get_dirty_pages_count(buf_pool, id, observer) == 0);
}

/** Remove all pages that belong to a given tablespace inside a specific
buffer pool instance when we are DISCARDing the tablespace.
@param[in,out]  buf_pool        buffer pool instance
@param[in]      id              space id */
static void buf_LRU_remove_all_pages(buf_pool_t *buf_pool, ulint id) {
scan_again:
  /* This administrative DISCARD scan waits out background promotion and
  compaction across the whole scan. Every path out releases this mutex. */
  mutex_enter(&buf_pool->LRU_drain_mutex);

  buf_pool->LRU_topology_latch.x_lock();

  auto all_freed = true;

  /* PS-11141 grouped LRU list: walk groups tail-to-head, then each
  group's pages. As with buf_LRU_drop_page_hash_for_tablespace() above,
  the AHI-drop path below restarts the whole scan (goto scan_again)
  rather than trying to resume a specific position across groups after
  releasing topology-X. Additionally: a successful removal below can
  empty and free `group` (if it was its last live page), so the next
  group to visit is captured into `next_group` *before* the inner loop
  touches group's pages, and the inner loop breaks immediately after any
  successful removal instead of continuing to read group->pages --
  otherwise both the inner loop and this outer loop's naive
  UT_LIST_GET_PREV(LRU, group) advance would dereference a freed group.
  next_group itself stays safe to use either way: we never mutate any
  group but the current one, so its identity is unaffected by our own
  removals. */
  for (buf_lru_group_t *group = UT_LIST_GET_LAST(buf_pool->LRU);
       group != nullptr;) {
    buf_lru_group_t *next_group = UT_LIST_GET_PREV(LRU, group);

    for (auto *bpage : group->pages) {
      if (bpage == nullptr) {
        continue;
      }

      rw_lock_t *hash_lock;
      BPageMutex *block_mutex;

      ut_a(buf_page_in_file(bpage));
      ut_ad(bpage->in_LRU_list);

      /* It is safe to check bpage->id.space() and bpage->io_fix
      while holding buf_pool->LRU_topology_latch only and later recheck
      while holding the buf_page_get_mutex() mutex.  */

      if (bpage->id.space() != id) {
        /* Skip this block, as it does not belong to
        the space that is being invalidated. */
        continue;
      } else if (bpage->was_io_fixed()) {
        /* We cannot remove this page during this scan
        yet; maybe the system is currently reading it
        in, or flushing the modifications to the file */

        all_freed = false;
        continue;
      } else {
        hash_lock = buf_page_hash_lock_get(buf_pool, bpage->id);

        rw_lock_x_lock(hash_lock, UT_LOCATION_HERE);

        block_mutex = buf_page_get_mutex(bpage);

        mutex_enter(block_mutex);

        if (bpage->id.space() != id || bpage->buf_fix_count > 0 ||
            (buf_page_get_io_fix(bpage) != BUF_IO_NONE)) {
          mutex_exit(block_mutex);

          rw_lock_x_unlock(hash_lock);

          /* We cannot remove this page during
          this scan yet; maybe the system is
          currently reading it in, or flushing
          the modifications to the file */

          all_freed = false;

          continue;
        }
      }

      ut_ad(mutex_own(block_mutex));

      DBUG_PRINT("ib_buf", ("evict page " UINT32PF ":" UINT32PF " state %u",
                            bpage->id.space(), bpage->id.page_no(),
                            static_cast<unsigned>(bpage->state)));

      if (buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
        /* Do nothing, because the adaptive hash index
        covers uncompressed pages only. */
      } else if (((buf_block_t *)bpage)->ahi.index) {
        buf_pool->LRU_topology_latch.x_unlock();
        mutex_exit(&buf_pool->LRU_drain_mutex);

        rw_lock_x_unlock(hash_lock);

        mutex_exit(block_mutex);

        /* Note that the following call will acquire
        and release block->lock X-latch.
        Note that the table cannot be evicted during
        the execution of ALTER TABLE...DISCARD TABLESPACE
        because MySQL is keeping the table handle open. */

        btr_search_drop_page_hash_when_freed(bpage->id, bpage->size);

        goto scan_again;
      } else {
        reinterpret_cast<buf_block_t *>(bpage)->ahi.assert_empty();
      }

      if (bpage->is_dirty()) {
        buf_flush_remove(bpage);
      }

      ut_ad(!bpage->in_flush_list);

      /* Remove from the LRU list. */

      if (buf_LRU_block_remove_hashed(bpage, true, false)) {
        buf_LRU_block_free_hashed_page((buf_block_t *)bpage);
      } else {
        ut_ad(block_mutex == &buf_pool->zip_mutex);
      }

      ut_ad(!mutex_own(block_mutex));

      /* buf_LRU_block_remove_hashed() releases the hash_lock */
      ut_ad(!rw_lock_own(hash_lock, RW_LOCK_X));
      ut_ad(!rw_lock_own(hash_lock, RW_LOCK_S));

      /* group may now be empty and freed: stop reading its pages and
      advance to next_group, captured above before any mutation, rather
      than dereferencing group again. This group's remaining pages, if
      it's still alive, are picked up on the outer all_freed retry loop
      at the bottom of this function. */
      break;
    }

    group = next_group;
  }

  buf_pool->LRU_topology_latch.x_unlock();
  mutex_exit(&buf_pool->LRU_drain_mutex);

  if (!all_freed) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    goto scan_again;
  }
}

/** Remove pages belonging to a given tablespace inside a specific
 buffer pool instance when we are deleting the data file(s) of that
 tablespace. The pages still remain a part of LRU and are evicted from
 the list as they age towards the tail of the LRU only if buf_remove
 is BUF_REMOVE_FLUSH_NO_WRITE. */
static void buf_LRU_remove_pages(
    buf_pool_t *buf_pool,    /*!< buffer pool instance */
    space_id_t id,           /*!< in: space id */
    buf_remove_t buf_remove, /*!< in: remove or flush strategy */
    const trx_t *trx,        /*!< to check if the operation must
                             be interrupted */
    bool strict)             /*!< in: true if no page from tablespace
                             can be in buffer pool just after flush */
{
  Flush_observer *observer = (trx == nullptr) ? nullptr : trx->flush_observer;

  switch (buf_remove) {
    case BUF_REMOVE_ALL_NO_WRITE:
      buf_LRU_remove_all_pages(buf_pool, id);
      break;

    case BUF_REMOVE_FLUSH_NO_WRITE:
      /* Pass trx as NULL to avoid interruption check. */
      buf_flush_dirty_pages(buf_pool, id, observer, false, nullptr, strict);
      break;

    case BUF_REMOVE_FLUSH_WRITE:
      buf_flush_dirty_pages(buf_pool, id, observer, true, trx, strict);

      if (observer == nullptr) {
        /* Ensure that all asynchronous IO is completed. */
        os_aio_wait_until_no_pending_writes();
        fil_flush(id);
      }
      break;

    case BUF_REMOVE_NONE:
      ut_error;
      break;
  }
}

void buf_LRU_flush_or_remove_pages(space_id_t id, buf_remove_t buf_remove,
                                   const trx_t *trx, bool strict) {
  /* Before we attempt to drop pages one by one we first
  attempt to drop page hash index entries in batches to make
  it more efficient. The batching attempt is a best effort
  attempt and does not guarantee that all pages hash entries
  will be dropped. We get rid of remaining page hash entries
  one by one below. */
  for (ulint i = 0; i < srv_buf_pool_instances; i++) {
    auto buf_pool = buf_pool_from_array(i);

    switch (buf_remove) {
      case BUF_REMOVE_ALL_NO_WRITE:
        buf_LRU_drop_page_hash_for_tablespace(buf_pool, id);
        break;

      case BUF_REMOVE_FLUSH_NO_WRITE:
        /* It is a DROP TABLE for a single table
        tablespace. No AHI entries exist because
        we already dealt with them when freeing up
        extents. */
      case BUF_REMOVE_FLUSH_WRITE:
        /* We allow read-only queries against the
        table, there is no need to drop the AHI entries. */
        break;

      case BUF_REMOVE_NONE:
        ut_error;
        break;
    }

    buf_LRU_remove_pages(buf_pool, id, buf_remove, trx, strict);
  }
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Insert a compressed block into buf_pool->zip_clean (PS-11141 grouped LRU
list). Pre-grouping, this inserted bpage at its actual LRU-order position
(found by walking buf_page_t's own intrusive LRU link to the nearest
zip_clean successor). Pages no longer link into buf_pool->LRU directly --
they belong to a buf_lru_group_t, which does not track relative order among
its own member pages -- so that position can no longer be determined; the
list's only consumers (buf_pool_validate_instance(), buf_all_freed())
iterate it in full regardless of order, so plain topology-X-ordered
insertion at the head is sufficient.
@param[in]      bpage   pointer to the block in question */
void buf_LRU_insert_zip_clean(buf_page_t *bpage) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);

  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(mutex_own(&buf_pool->zip_mutex));
  ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_PAGE);

  UT_LIST_ADD_FIRST(buf_pool->zip_clean, bpage);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/** Try to free an uncompressed page of a compressed block from the unzip
LRU list.  The compressed page is preserved, and it need not be clean.
@param[in]      buf_pool        buffer pool instance
@param[in]      scan_all        scan whole LRU list if true, otherwise
                                scan only srv_LRU_scan_depth / 2 blocks
@return true if freed */
static bool buf_LRU_free_from_unzip_LRU_list(buf_pool_t *buf_pool,
                                             bool scan_all) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  if (!buf_LRU_evict_from_unzip_LRU(buf_pool)) {
    return (false);
  }

  ulint scanned = 0;
  bool freed = false;

  for (buf_block_t *block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);
       block != nullptr && (scan_all || scanned < srv_LRU_scan_depth);
       ++scanned) {
    buf_block_t *prev_block;

    prev_block = UT_LIST_GET_PREV(unzip_LRU, block);

    mutex_enter(&block->mutex);

    ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
    ut_ad(block->in_unzip_LRU_list);
    ut_ad(block->page.in_LRU_list);

    freed = buf_LRU_free_page(&block->page, false);

    if (freed) {
      ++scanned;
      break;
    }

    mutex_exit(&block->mutex);
    block = prev_block;
  }

  if (scanned) {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_UNZIP_SEARCH_SCANNED,
                                 MONITOR_LRU_UNZIP_SEARCH_SCANNED_NUM_CALL,
                                 MONITOR_LRU_UNZIP_SEARCH_SCANNED_PER_CALL,
                                 scanned);
  }

  return (freed);
}

/** Try to free a clean page from the common LRU list.
@param[in,out]  buf_pool        buffer pool instance
@param[in]      scan_all        scan whole LRU list if true, otherwise scan
                                only up to BUF_LRU_SEARCH_SCAN_THRESHOLD
@return true if freed */
/** Scans buf_pool->LRU tail-to-head with identity-based optimistic eviction
(PS-11141): snapshot one group's (page_id, residency_generation) values under
a short topology-X section, release it for provisional inspection, then
commit through the existing free paths after full revalidation. The group
hazard pointer (lru_scan_itr) protects the next position across that window. */
static bool buf_LRU_provisionally_replaceable(const buf_page_t *bpage) {
  ut_ad(mutex_own(buf_page_get_mutex(bpage)));
  if (!buf_page_in_file(bpage)) {
    return false;
  }
  /* Deliberately not buf_page_can_relocate(): its
  ut_ad(bpage->in_LRU_list || io_fix == BUF_IO_READ) assumes the caller
  holds topology-X, which this provisional (lock-dropped) check does
  not. A concurrent make-young/promotion reposition
  (buf_LRU_remove_block() + buf_LRU_add_block_low(), via
  buf_LRU_detach_from_group()) protects in_LRU_list under topology-X
  alone, not this page's block mutex, so it can transiently clear
  in_LRU_list on this exact page without evicting it at all -- observing
  that here is expected, not a bug. The verdict below is re-verified
  under topology-X before anything commits (see
  buf_LRU_try_evict_tail_identity()), so a stale answer either way is
  harmless. */
  if (buf_page_get_io_fix(bpage) != BUF_IO_NONE || bpage->buf_fix_count != 0) {
    return false;
  }
  if (bpage->was_stale()) {
    return true;
  }
  return !bpage->is_dirty();
}

/** Releases whichever topology mode the caller currently holds (PS-11141
Requirement 8): buf_LRU_free_page() and buf_LRU_try_evict_tail_identity() may
be entered under either mode, so their internal unlocks must match instead
of assuming topology-X.
@param[in,out]  buf_pool  buffer pool instance */
static inline void buf_lru_topology_unlock(buf_pool_t *buf_pool) {
  if (buf_pool->LRU_topology_latch.owns_x()) {
    buf_pool->LRU_topology_latch.x_unlock();
  } else {
    buf_pool->LRU_topology_latch.s_unlock();
  }
}

/** Try to evict one snapshotted tail identity after the topology latch was
dropped. On success, topology-X is released by the free path. On failure,
neither topology-X nor the block mutex is held.
@param[in,out] buf_pool buffer pool instance
@param[in] identity page identity from the snapshot
@param[in] exhaustive whether this is a quiesced invalidation scan
@return true if a page was freed */
static bool buf_LRU_try_evict_tail_identity(buf_pool_t *buf_pool,
                                            const buf_lru_promote_t &identity,
                                            bool exhaustive) {
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());

  rw_lock_t *hash_lock = nullptr;
  buf_page_t *bpage =
      buf_page_hash_get_s_locked(buf_pool, identity.page_id, &hash_lock);
  if (bpage == nullptr) {
    return false;
  }

  if (buf_pool_watch_is_sentinel(buf_pool, bpage) ||
      bpage->residency_generation != identity.residency_generation ||
      !buf_page_in_file(bpage)) {
    rw_lock_s_unlock(hash_lock);
    return false;
  }

  auto *block_mutex = buf_page_get_mutex(bpage);
  if (exhaustive) {
    mutex_enter(block_mutex);
  } else if (mutex_enter_nowait(block_mutex) != 0) {
    rw_lock_s_unlock(hash_lock);
    return false;
  }

  const bool promising =
      bpage->id == identity.page_id &&
      bpage->residency_generation == identity.residency_generation &&
      buf_LRU_provisionally_replaceable(bpage);

  mutex_exit(block_mutex);
  rw_lock_s_unlock(hash_lock);

  if (!promising) {
    return false;
  }

  /* PS-11141 Requirement 8: try topology-S first for the common case (a
  plain, uncompressed FILE_PAGE, once the LRU is long enough that
  buf_LRU_remove_block()'s topology-S path may skip
  buf_LRU_old_adjust_len() -- see its doc comment). Warm-up is rare and
  cheap to check without any lock. Every
  other disqualifying condition (unzip_LRU membership, a still-attached
  zip.data, a stale page needing buf_page_free_stale()'s topology-X-only
  path) can only be observed once the page is re-resolved and its block
  mutex held below, so on that discovery this retries once under
  topology-X instead of trying to upgrade -- no upgrade path exists between
  S and X. */
  bool try_s = buf_pool->LRU_n_pages.load(std::memory_order_relaxed) >
               BUF_LRU_OLD_MIN_LEN;

  for (;;) {
    /* Re-resolve under the topology latch. Do not retain the earlier
    descriptor pointer: the page may have been evicted and the identity
    reused. */
    if (try_s) {
      buf_pool->LRU_topology_latch.s_lock();
    } else {
      buf_pool->LRU_topology_latch.x_lock();
    }
    bpage = buf_page_hash_get_s_locked(buf_pool, identity.page_id, &hash_lock);
    if (bpage == nullptr || buf_pool_watch_is_sentinel(buf_pool, bpage) ||
        bpage->residency_generation != identity.residency_generation ||
        !buf_page_in_file(bpage) || bpage->lru_group == nullptr) {
      if (bpage != nullptr) {
        rw_lock_s_unlock(hash_lock);
      }
      buf_lru_topology_unlock(buf_pool);
      return false;
    }

    /* Keep-zip eviction can replace the descriptor while preserving
    residency_generation; rebind the page mutex to the current object. */
    block_mutex = buf_page_get_mutex(bpage);
    if (exhaustive) {
      mutex_enter(block_mutex);
    } else if (mutex_enter_nowait(block_mutex) != 0) {
      rw_lock_s_unlock(hash_lock);
      buf_lru_topology_unlock(buf_pool);
      return false;
    }
    rw_lock_s_unlock(hash_lock);

    if (try_s && (bpage->was_stale() || bpage->zip.data != nullptr ||
                  buf_page_belongs_to_unzip_LRU(bpage))) {
      /* Compressed-page and stale-page eviction stay topology-X only
      (Step 5 deferral; buf_page_free_stale() itself asserts owns_x()).
      Release everything and retry from scratch under topology-X: no
      upgrade path exists between S and X. */
      mutex_exit(block_mutex);
      buf_pool->LRU_topology_latch.s_unlock();
      try_s = false;
      continue;
    }

    break;
  }

  bool freed = false;
  if (bpage->id == identity.page_id &&
      bpage->residency_generation == identity.residency_generation &&
      buf_page_in_file(bpage) && bpage->lru_group != nullptr) {
    if (bpage->was_stale()) {
      mutex_exit(block_mutex);
      freed = buf_page_free_stale(buf_pool, bpage);
    } else if (buf_flush_ready_for_replace(bpage)) {
      const auto accessed = buf_page_is_accessed(bpage);
      freed = buf_LRU_free_page(bpage, true);
      if (freed && accessed == std::chrono::steady_clock::time_point{}) {
        ++buf_pool->stat.n_ra_pages_evicted;
      }
      if (!freed) {
        mutex_exit(block_mutex);
      }
    } else {
      mutex_exit(block_mutex);
    }
  } else {
    mutex_exit(block_mutex);
  }

  if (!freed) {
    buf_lru_topology_unlock(buf_pool);
  }

  ut_ad(!mutex_own(block_mutex));
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());
  return freed;
}

static bool buf_LRU_free_from_common_LRU_list(buf_pool_t *buf_pool,
                                              bool scan_all, bool exhaustive) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  ulint scanned{};
  buf_lru_group_t *group = buf_pool->lru_scan_itr.start();

  while (group != nullptr &&
         (scan_all || scanned < BUF_LRU_SEARCH_SCAN_THRESHOLD)) {
    ut_ad(buf_pool->LRU_topology_latch.owns_x());

    auto prev_group = UT_LIST_GET_PREV(LRU, group);
    buf_pool->lru_scan_itr.set(prev_group);

    std::array<std::optional<buf_lru_promote_t>, BUF_LRU_GROUP_SIZE>
        candidates{};
    uint32_t candidate_count = 0;

    for (uint32_t slot = 0; slot < BUF_LRU_GROUP_SIZE; ++slot) {
      buf_page_t *bpage = group->pages[slot];
      if (bpage == nullptr) {
        continue;
      }
      if (!scan_all &&
          scanned + candidate_count >= BUF_LRU_SEARCH_SCAN_THRESHOLD) {
        break;
      }
      candidates[candidate_count] =
          buf_lru_promote_t{bpage->id, bpage->residency_generation};
      ++candidate_count;
    }

    /* Drop the list mutex for provisional inspection of this group. */
    buf_pool->LRU_topology_latch.x_unlock();

    bool freed = false;
    for (uint32_t i = 0; i < candidate_count; ++i) {
      ++scanned;
      if (buf_LRU_try_evict_tail_identity(buf_pool, *candidates[i],
                                          exhaustive)) {
        freed = true;
        break;
      }
      if (!scan_all && scanned >= BUF_LRU_SEARCH_SCAN_THRESHOLD) {
        break;
      }
    }

    if (freed) {
      if (scanned) {
        MONITOR_INC_VALUE_CUMULATIVE(
            MONITOR_LRU_SEARCH_SCANNED, MONITOR_LRU_SEARCH_SCANNED_NUM_CALL,
            MONITOR_LRU_SEARCH_SCANNED_PER_CALL, scanned);
      }
      ut_ad(!buf_pool->LRU_topology_latch.owns_x());
      return true;
    }

    if (!scan_all && scanned >= BUF_LRU_SEARCH_SCAN_THRESHOLD) {
      buf_pool->LRU_topology_latch.x_lock();
      break;
    }

    buf_pool->LRU_topology_latch.x_lock();
    group = buf_pool->lru_scan_itr.get();
  }

  if (scanned) {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_SEARCH_SCANNED,
                                 MONITOR_LRU_SEARCH_SCANNED_NUM_CALL,
                                 MONITOR_LRU_SEARCH_SCANNED_PER_CALL, scanned);
  }

  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  return false;
}

bool buf_LRU_scan_and_free_block(buf_pool_t *buf_pool, bool scan_all,
                                 bool exhaustive) {
  bool freed = false;
  bool owns_common_scan = false;
  bool use_unzip_list = UT_LIST_GET_LEN(buf_pool->unzip_LRU) > 0;

  if (scan_all) {
    buf_pool->LRU_scan_owner.acquire();
    owns_common_scan = true;
  }

  buf_pool->LRU_topology_latch.x_lock();

  if (use_unzip_list) {
    freed = buf_LRU_free_from_unzip_LRU_list(buf_pool, scan_all);
  }

  if (!freed) {
    if (!owns_common_scan) {
      owns_common_scan = buf_pool->LRU_scan_owner.try_acquire();
    }
    if (owns_common_scan) {
      freed = buf_LRU_free_from_common_LRU_list(buf_pool, scan_all, exhaustive);
    }
  }

  if (!freed) {
    buf_pool->LRU_topology_latch.x_unlock();
  }

  ut_ad(!buf_pool->LRU_topology_latch.owns_x());

  if (owns_common_scan) {
    buf_pool->LRU_scan_owner.release();
  }

  return (freed);
}

/** Returns true if less than 25 % of the buffer pool in any instance is
 available. This can be used in heuristics to prevent huge transactions
 eating up the whole buffer pool for their locks.
 @return true if less than 25 % of buffer pool left */
bool buf_LRU_buf_pool_running_out(void) {
  bool ret = false;

  for (ulint i = 0; i < srv_buf_pool_instances && !ret; i++) {
    buf_pool_t *buf_pool;

    buf_pool = buf_pool_from_array(i);

    if (!recv_recovery_is_on() &&
        UT_LIST_GET_LEN(buf_pool->free) + buf_pool->LRU_n_pages <
            std::min(buf_pool->curr_size, buf_pool->old_size) / 4) {
      ret = true;
    }
  }

  return ret;
}

/** Returns a free block from the buf_pool.
The block is taken off the free list.  If it is empty, returns NULL.
@param[in]      buf_pool        buffer pool instance
@return a free control block, or NULL if the buf_block->free list is empty */
buf_block_t *buf_LRU_get_free_only(buf_pool_t *buf_pool) {
  buf_block_t *block;

  mutex_enter(&buf_pool->free_list_mutex);

  block = reinterpret_cast<buf_block_t *>(UT_LIST_GET_FIRST(buf_pool->free));

  while (block != nullptr) {
    ut_ad(block->page.in_free_list);
    ut_d(block->page.in_free_list = false);
    ut_ad(!block->page.in_flush_list);
    ut_ad(!block->page.in_LRU_list);
    ut_a(!buf_page_in_file(&block->page));
    UT_LIST_REMOVE(buf_pool->free, &block->page);
    mutex_exit(&buf_pool->free_list_mutex);

    if (!buf_get_withdraw_depth(buf_pool) ||
        !buf_block_will_withdrawn(buf_pool, block)) {
      /* found valid free block */
      /* No adaptive hash index entries may point to
      a free block. */
      block->ahi.assert_empty();

      buf_block_set_state(block, BUF_BLOCK_READY_FOR_USE);

      UNIV_MEM_ALLOC(block->frame, UNIV_PAGE_SIZE);

      ut_ad(buf_pool_from_block(block) == buf_pool);

      /* Initialize latches on first use. The block has just been removed from
      the free list and is owned exclusively by this thread, so this access is
      race-free; the prior thread's initialization (if any) is visible through
      the free_list_mutex handoff. No mutex protects this access - exclusive
      ownership does, hence a relaxed load. (The flag is std::atomic only for
      the lock-free I_S buffer-page scanner; see buf0buf.h.) */
      ut_ad(buf_block_get_state(block) == BUF_BLOCK_READY_FOR_USE);
      ut_ad(!block->page.in_free_list);
      if (!block->latches_initialized.load(std::memory_order_relaxed)) {
        buf_block_lazy_init_latches(block);
      }

      return (block);
    }

    /* This should be withdrawn */
    mutex_enter(&buf_pool->free_list_mutex);
    UT_LIST_ADD_LAST(buf_pool->withdraw, &block->page);
    ut_d(block->in_withdraw_list = true);

    block = reinterpret_cast<buf_block_t *>(UT_LIST_GET_FIRST(buf_pool->free));
  }

  mutex_exit(&buf_pool->free_list_mutex);

  return (block);
}

/** Checks how much of buf_pool is occupied by non-data objects like
 AHI, lock heaps etc. Depending on the size of non-data objects this
 function will either assert or issue a warning and switch on the
 status monitor. */
static void buf_LRU_check_size_of_non_data_objects(
    const buf_pool_t *buf_pool) /*!< in: buffer pool instance */
{
  const size_t mb = (buf_pool->curr_size / (1024 * 1024 / UNIV_PAGE_SIZE));

  if (!recv_recovery_is_on() && buf_pool->curr_size == buf_pool->old_size &&
      UT_LIST_GET_LEN(buf_pool->free) + buf_pool->LRU_n_pages <
          buf_pool->curr_size / 20) {
    const bool buf_pool_full = true;
    LogErr(ERROR_LEVEL, ER_IB_BUFFER_POOL_FULL,
           "buf_LRU_check_size_of_non_data_objects()", mb);
    ut_a(!buf_pool_full);

  } else if (!recv_recovery_is_on() &&
             buf_pool->curr_size == buf_pool->old_size &&
             (UT_LIST_GET_LEN(buf_pool->free) + buf_pool->LRU_n_pages) <
                 buf_pool->curr_size / 3) {
    if (!buf_lru_switched_on_innodb_mon.exchange(true)) {
      /* Over 67 % of the buffer pool is occupied by lock heaps or the adaptive
      hash index or BUF_BLOCK_MEMORY pages. This may be a memory leak! */

      LogErr(WARNING_LEVEL, ER_IB_BUFFER_POOL_OVERUSE,
             "buf_LRU_check_size_of_non_data_objects()", mb);
      srv_innodb_needs_monitoring++;
    }

  } else if (buf_lru_switched_on_innodb_mon.load()) {
    if (buf_lru_switched_on_innodb_mon.exchange(false)) {
      srv_innodb_needs_monitoring--;
    }
  }
}

/** Diagnose failure to get a free page and request InnoDB monitor output in
the error log if more than two seconds have been spent already.
@param[in]	n_iterations	how many buf_LRU_get_free_page iterations
already completed
@param[in]	started_time	timestamp of when the attempt to get the
free page started
@param[in]	flush_failures	how many times single-page flush, if allowed,
has failed
@param[in,out]	started_monitor	whether InnoDB monitor print has been requested
*/
static void buf_LRU_handle_lack_of_free_blocks(
    ulint n_iterations, std::chrono::steady_clock::time_point started_time,
    ulint flush_failures, bool *started_monitor) {
  static std::chrono::steady_clock::time_point last_printout_time;

  /* Legacy algorithm started warning after at least 2 seconds, we
  emulate this. */
  const auto current_time = std::chrono::steady_clock::now();
  const std::chrono::milliseconds limit{2000};

  if ((current_time - started_time > limit) &&
      (current_time - last_printout_time > limit) &&
      srv_buf_pool_old_size == srv_buf_pool_size) {
    ib::warn(ER_IB_MSG_134)
        << "Difficult to find free blocks in the buffer pool"
           " ("
        << n_iterations << " search iterations)! " << flush_failures
        << " failed attempts to"
           " flush a page! Consider increasing the buffer pool"
           " size. It is also possible that in your Unix version"
           " fsync is very slow, or completely frozen inside"
           " the OS kernel. Then upgrading to a newer version"
           " of your operating system may help. Look at the"
           " number of fsyncs in diagnostic info below."
           " Pending flushes (fsync) log: "
        << log_pending_flushes()
        << "; buffer pool: " << fil_n_pending_tablespace_flushes << ". "
        << os_n_file_reads << " OS file reads, " << os_n_file_writes
        << " OS file writes, " << os_n_fsyncs
        << " OS fsyncs. Starting InnoDB Monitor to print"
           " further diagnostics to the standard output.";

    last_printout_time = current_time;

    if (!*started_monitor) {
      *started_monitor = true;
      srv_innodb_needs_monitoring++;
    }
  }
}

/** The maximum allowed backoff sleep time duration, microseconds */
static constexpr auto MAX_FREE_LIST_BACKOFF_SLEEP = 10000;

/** The sleep reduction factor for high-priority waiter backoff sleeps */
static constexpr auto FREE_LIST_BACKOFF_HIGH_PRIO_DIVIDER = 100;

/** The sleep reduction factor for low-priority waiter backoff sleeps */
static constexpr auto FREE_LIST_BACKOFF_LOW_PRIO_DIVIDER = 1;

/** Returns a free block from the buf_pool. The block is taken off the
free list. If free list is empty, blocks are moved from the end of the
LRU list to the free list.
This function is called from a user thread when it needs a clean
block to read in a page. Note that we only ever get a block from
the free list. Even when we flush a page or find a page in LRU scan
we put it to free list to be used.
* iteration 0:
  * get a block from free list, success:done
  * if buf_pool->try_LRU_scan is set
    * scan LRU up to srv_LRU_scan_depth to find a clean block
    * the above will put the block on free list
    * success:retry the free list
  * flush one dirty page from tail of LRU to disk
    * the above will put the block on free list
    * success: retry the free list
* iteration 1:
  * same as iteration 0 except:
    * scan whole LRU list
    * scan LRU list even if buf_pool->try_LRU_scan is not set
* iteration > 1:
  * same as iteration 1 but sleep 10ms
@param[in,out]  buf_pool        buffer pool instance
@return the free control block, in state BUF_BLOCK_READY_FOR_USE */
buf_block_t *buf_LRU_get_free_block(buf_pool_t *buf_pool) {
  buf_block_t *block = nullptr;
  bool freed = false, no_flush_waited = false;
  ulint n_iterations = 0;
  ulint flush_failures = 0;
  bool started_monitor = false;
  std::chrono::steady_clock::time_point started_time;

  ut_ad(!buf_pool->LRU_topology_latch.owns_x());

  MONITOR_INC(MONITOR_LRU_GET_FREE_SEARCH);
loop:
  buf_LRU_check_size_of_non_data_objects(buf_pool);

  /* If there is a block in the free list, take it */
  if (DBUG_EVALUATE_IF("simulate_lack_of_pages", true, false)) {
    block = NULL;

    if (srv_debug_monitor_printed) DBUG_SET("-d,simulate_lack_of_pages");

  } else if (DBUG_EVALUATE_IF("simulate_recovery_lack_of_pages",
                              recv_recovery_on, false)) {
    block = NULL;

    if (srv_debug_monitor_printed) {
      flush_error_log_messages();
      DBUG_SUICIDE();
    }
  } else {
    block = buf_LRU_get_free_only(buf_pool);
  }

  if (block != nullptr) {
    ut_ad(!block->page.someone_has_io_responsibility());
    ut_ad(buf_pool_from_block(block) == buf_pool);
    memset(&block->page.zip, 0, sizeof block->page.zip);

    if (started_monitor) {
      srv_innodb_needs_monitoring--;
    }

    block->page.reset_flush_observer();
    return block;
  }

  if (started_time == std::chrono::steady_clock::time_point{})
    started_time = std::chrono::steady_clock::now();

  MONITOR_INC(MONITOR_LRU_GET_FREE_LOOPS);

  freed = false;

  if (srv_lru_threads_enabled) {
    os_event_set(buf_pool->lru_manager_event);
  } else if (!srv_read_only_mode) {
    os_event_set(buf_flush_event);
  }

  if (srv_empty_free_list_algorithm == SRV_EMPTY_FREE_LIST_BACKOFF &&
      buf_flush_page_cleaner_is_active() &&
      (srv_shutdown_state.load() == SRV_SHUTDOWN_NONE ||
       srv_shutdown_state.load() == SRV_SHUTDOWN_CLEANUP)) {
    /* Backoff to minimize the free list mutex contention while the free list
    is empty */
    const auto priority = srv_current_thread_priority;

    if (n_iterations < 3) {
      std::this_thread::yield();
      if (!priority) {
        std::this_thread::yield();
      }
    } else {
      ulint i, b;

      if (n_iterations < 6) {
        i = n_iterations - 3;
      } else if (n_iterations < 8) {
        i = 4;
      } else if (n_iterations < 11) {
        i = 5;
      } else {
        i = n_iterations - 5;
      }
      b = 1 << i;
      if (b > MAX_FREE_LIST_BACKOFF_SLEEP) {
        b = MAX_FREE_LIST_BACKOFF_SLEEP;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(
          b / (priority ? FREE_LIST_BACKOFF_HIGH_PRIO_DIVIDER
                        : FREE_LIST_BACKOFF_LOW_PRIO_DIVIDER)));
    }

    buf_LRU_handle_lack_of_free_blocks(n_iterations, started_time,
                                       flush_failures, &started_monitor);

    n_iterations++;

    srv_stats.buf_pool_wait_free.add(n_iterations, 1);

    /* In case of backoff, do not ever attempt single page flushes and
    wait for the cleaner to free some pages instead.  */
    goto loop;
  } else {
    /* The LRU manager is not running or Oracle MySQL 5.6 algorithm
    was requested, will perform a single page flush  */
    ut_ad((srv_empty_free_list_algorithm == SRV_EMPTY_FREE_LIST_LEGACY) ||
          !buf_flush_page_cleaner_is_active() ||
          (srv_shutdown_state.load() != SRV_SHUTDOWN_NONE &&
           srv_shutdown_state.load() != SRV_SHUTDOWN_CLEANUP));
  }

  os_rmb;

  if (DBUG_EVALUATE_IF("simulate_recovery_lack_of_pages", true, false) ||
      DBUG_EVALUATE_IF("simulate_lack_of_pages", true, false))
    buf_pool->try_LRU_scan = false;

  if (buf_pool->try_LRU_scan || n_iterations > 0) {
    /* If no block was in the free list, search from the
    end of the LRU list and try to free a block there.
    If we are doing for the first time we'll scan only
    tail of the LRU list otherwise we scan the whole LRU
    list. */
    freed = buf_LRU_scan_and_free_block(buf_pool, n_iterations > 0);

    if (!freed && n_iterations == 0) {
      /* Tell other threads that there is no point
      in scanning the LRU list. This flag is set to
      true again when we flush a batch from this
      buffer pool. */
      buf_pool->try_LRU_scan = false;
      os_wmb;
    }
  }

  if (freed) {
    goto loop;
  }

  if (n_iterations > 20 && srv_buf_pool_old_size == srv_buf_pool_size) {
    ib::warn(ER_IB_MSG_134)
        << "Difficult to find free blocks in the buffer pool"
           " ("
        << n_iterations << " search iterations)! " << flush_failures
        << " failed attempts to"
           " flush a page! Consider increasing the buffer pool"
           " size. It is also possible that in your Unix version"
           " fsync is very slow, or completely frozen inside"
           " the OS kernel. Then upgrading to a newer version"
           " of your operating system may help. Look at the"
           " number of fsyncs in diagnostic info below."
           " Pending flushes (fsync) log: "
        << log_pending_flushes()
        << "; buffer pool: " << fil_n_pending_tablespace_flushes << ". "
        << os_n_file_reads << " OS file reads, " << os_n_file_writes
        << " OS file writes, " << os_n_fsyncs
        << " OS fsyncs. Starting InnoDB Monitor to print"
           " further diagnostics to the standard output.";
    if (!started_monitor) {
      started_monitor = true;
      srv_innodb_needs_monitoring++;
    }
  }

  /* If we have scanned the whole LRU and still are unable to
  find a free block then we should sleep here to let the
  page_cleaner do an LRU batch for us. */

  if (n_iterations > 1) {
    MONITOR_INC(MONITOR_LRU_GET_FREE_WAITS);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  /* No free block was found: try to flush the LRU list.
  This call will flush one page from the LRU and put it on the
  free list. That means that the free block is up for grabs for
  all user threads.

  TODO: A more elegant way would have been to return the freed
  up block to the caller here but the code that deals with
  removing the block from page_hash and LRU_list is fairly
  involved (particularly in case of compressed pages). We
  can do that in a separate patch sometime in future. */

  if (srv_lru_threads_enabled && buf_pool->init_flush[BUF_FLUSH_LRU] &&
      dblwr::is_enabled() && buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE] >= 1 &&
      !no_flush_waited) {
    /* Cap reached: wait for an in-progress LRU flush instead of issuing
    our own single-page flush. */
    MONITOR_INC(MONITOR_LRU_FLUSH_AWAIT_COUNT);
    buf_flush_await_no_flushing(buf_pool, BUF_FLUSH_LRU);
    no_flush_waited = true;
  } else {
    /* Below the cap: issue our own single-page flush. */
    MONITOR_INC(MONITOR_LRU_SINGLE_PAGE_FLUSH_COUNT);
    if (!buf_flush_single_page_from_LRU(buf_pool)) {
      MONITOR_INC(MONITOR_LRU_SINGLE_FLUSH_FAILURE_COUNT);
      ++flush_failures;
    }
  }

  srv_stats.buf_pool_wait_free.add(n_iterations, 1);

  n_iterations++;

  goto loop;
}

/** Calculates the desired number for the old blocks list, in pages.
Note: UT_LIST_GET_LEN(buf_pool->LRU) is a GROUP count (PS-11141 grouped LRU
list); the page count is buf_pool->LRU_n_pages.
@param[in]      buf_pool        buffer pool instance */
static size_t calculate_desired_LRU_old_size(const buf_pool_t *buf_pool) {
  return std::min(buf_pool->LRU_n_pages *
                      static_cast<size_t>(buf_pool->LRU_old_ratio) /
                      BUF_LRU_OLD_RATIO_DIV,
                  buf_pool->LRU_n_pages -
                      (BUF_LRU_OLD_TOLERANCE + BUF_LRU_NON_OLD_MIN_LEN));
}

/** Moves the LRU_old pointer so that the length of the old blocks list
is inside the allowed limits (PS-11141 grouped LRU list).

The boundary now moves in whole-GROUP steps (up to BUF_LRU_GROUP_SIZE pages
at a time), so naively porting the page-level fixed-point loop -- move one
step, stop once within +/-BUF_LRU_OLD_TOLERANCE -- can hang: a single group
move can overshoot the (page-sized) tolerance band, flipping the loop's
direction on the very next iteration, forever, inside this topology-X-
held call. Instead, a group is only moved across the boundary when doing so
strictly reduces the absolute imbalance |old_len - new_len|; if neither
direction would improve on it, the loop stops even outside the tolerance
band. This terminates regardless of tolerance sizing, since each successful
move strictly shrinks a bounded non-negative integer.
@param[in]      buf_pool        buffer pool instance */
static inline void buf_LRU_old_adjust_len(buf_pool_t *buf_pool) {
  ut_a(buf_pool->LRU_old);
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(buf_pool->LRU_old_ratio >= BUF_LRU_OLD_RATIO_MIN);
  ut_ad(buf_pool->LRU_old_ratio <= BUF_LRU_OLD_RATIO_MAX);
  static_assert(BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN >
                    BUF_LRU_OLD_RATIO_DIV * (BUF_LRU_OLD_TOLERANCE + 5),
                "BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN <= "
                "BUF_LRU_OLD_RATIO_DIV * (BUF_LRU_OLD_TOLERANCE + 5)");
#ifdef UNIV_LRU_DEBUG
  /* buf_pool->LRU_old must be the first group in the LRU list whose "old"
  flag is set. */
  ut_a(buf_pool->LRU_old->old);
  ut_a(!UT_LIST_GET_PREV(LRU, buf_pool->LRU_old) ||
       !UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)->old);
  ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old) ||
       UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */

  size_t old_len = buf_pool->LRU_old_len;
  const size_t new_len = calculate_desired_LRU_old_size(buf_pool);
  auto abs_diff = [](size_t a, size_t b) { return a > b ? a - b : b - a; };

  ut_d(size_t iterations = 0);

  for (;;) {
    ut_ad(++iterations <= UT_LIST_GET_LEN(buf_pool->LRU) + 1);

    if (abs_diff(old_len, new_len) <= BUF_LRU_OLD_TOLERANCE) {
      return;
    }

    buf_lru_group_t *LRU_old = buf_pool->LRU_old;
    ut_a(LRU_old);
#ifdef UNIV_LRU_DEBUG
    ut_a(LRU_old->old);
#endif /* UNIV_LRU_DEBUG */

    if (old_len < new_len) {
      /* Grow the old sublist: try to pull in LRU_old's predecessor group. */
      buf_lru_group_t *prev = UT_LIST_GET_PREV(LRU, LRU_old);
      if (prev == nullptr) {
        /* No more groups on the young side to pull in. */
        return;
      }

      const size_t candidate_len = old_len + prev->n_pages;
      /* Crossing an empty group costs nothing, so always cross it rather
      than rejecting a no-op tie and getting stuck. */
      if (prev->n_pages > 0 &&
          abs_diff(candidate_len, new_len) >= abs_diff(old_len, new_len)) {
        /* This move would not improve (or would tie); stop here rather
        than oscillate across a group-sized step forever. */
        return;
      }

      prev->old = true;
      for (auto *bpage : prev->pages) {
        if (bpage != nullptr) {
          buf_page_set_old(bpage, true);
        }
      }
      buf_pool->LRU_old = prev;
      old_len = candidate_len;
      buf_pool->LRU_old_len = old_len;
    } else {
      /* Shrink the old sublist: push LRU_old itself out to the young side. */
      buf_lru_group_t *next = UT_LIST_GET_NEXT(LRU, LRU_old);
      if (next == nullptr) {
        /* LRU_old is the tail group; nothing left to shrink into. */
        return;
      }

      const size_t candidate_len = old_len - LRU_old->n_pages;
      /* See the matching comment in the grow branch above: an
      empty-but-still-linked LRU_old group must always be crossed, since
      the strictly-improving-move test would otherwise reject the
      zero-cost move as a tie and get stuck. */
      if (LRU_old->n_pages > 0 &&
          abs_diff(candidate_len, new_len) >= abs_diff(old_len, new_len)) {
        return;
      }

      LRU_old->old = false;
      for (auto *bpage : LRU_old->pages) {
        if (bpage != nullptr) {
          buf_page_set_old(bpage, false);
        }
      }
      buf_pool->LRU_old = next;
      old_len = candidate_len;
      buf_pool->LRU_old_len = old_len;
    }
  }
}

/** Initializes the old blocks pointer in the LRU list. This function should be
called when the LRU list grows to BUF_LRU_OLD_MIN_LEN pages (PS-11141
grouped LRU list).
@param[in,out]  buf_pool        buffer pool instance */
static void buf_LRU_old_init(buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_a(buf_pool->LRU_n_pages == BUF_LRU_OLD_MIN_LEN);

  /* We first initialize all groups (and their pages) in the LRU list as
  old and then use the adjust function to move the LRU_old pointer to the
  right position. */

  /* Sum each group's own n_pages while marking it old. */
  size_t old_len = 0;

  for (auto *group : buf_pool->LRU) {
    group->old = true;
    old_len += group->n_pages;
    for (auto *bpage : group->pages) {
      if (bpage != nullptr) {
        ut_ad(buf_page_in_file(bpage));
        /* This loop temporarily violates the assertions of
        buf_page_set_old(). */
        bpage->old.store(true, std::memory_order_relaxed);
      }
    }
  }

  buf_pool->LRU_old = UT_LIST_GET_FIRST(buf_pool->LRU);
  buf_pool->LRU_old_len = old_len;

  buf_LRU_old_adjust_len(buf_pool);
}

/** Remove a block from the unzip_LRU list if it belonged to the list.
@param[in]      bpage   control block */
static void buf_unzip_LRU_remove_block_if_needed(buf_page_t *bpage) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);

  ut_ad(buf_page_in_file(bpage));
  /* PS-11141 Requirement 8: buf_LRU_remove_block_finish() may now be
  reached under topology-S (via buf_LRU_remove_block()'s topology-S fast
  path). unzip_LRU itself remains topology-X-only territory (Requirement
  11's dedicated latch is not yet implemented; deferred throughout this
  effort, see Step 5), so the topology-S caller must have already ruled
  out buf_page_belongs_to_unzip_LRU(bpage) -- this call is then a
  structural no-op, and the relaxed assertion below only documents that
  either mode is safe to enter with, not that unzip_LRU mutation itself
  runs under S. */
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(buf_pool->LRU_topology_latch.owns_x() ||
        !buf_page_belongs_to_unzip_LRU(bpage));

  if (buf_page_belongs_to_unzip_LRU(bpage)) {
    buf_block_t *block = reinterpret_cast<buf_block_t *>(bpage);

    ut_ad(block->in_unzip_LRU_list);
    ut_d(block->in_unzip_LRU_list = false);

    UT_LIST_REMOVE(buf_pool->unzip_LRU, block);
  }
}

/** Adjust LRU group hazard pointers if needed (PS-11141 grouped LRU list).
@param[in] buf_pool Buffer pool instance
@param[in] group Group about to be unlinked */
void buf_LRU_adjust_group_hp(buf_pool_t *buf_pool,
                             const buf_lru_group_t *group) {
  buf_pool->lru_hp.adjust(group);
  buf_pool->lru_scan_itr.adjust(group);
  buf_pool->single_scan_itr.adjust(group);
  buf_pool->lru_compact_hp.adjust(group);
  buf_pool->LRU_empty_scan_cursor.adjust(group);
}

/** Replace bpage's slot in its LRU group with dpage, without moving the
group itself (PS-11141 grouped LRU list).
@param[in]      bpage   old page descriptor, currently linked into a group
@param[in,out]  dpage   new page descriptor, taking over bpage's slot */
void buf_LRU_relocate_in_group(buf_page_t *bpage, buf_page_t *dpage) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  buf_lru_group_t *group = bpage->lru_group;
  ut_a(group);

  ut_ad(group->pages[bpage->lru_slot] == bpage);
  ut_ad(group->occupied_slots & (uint32_t{1} << bpage->lru_slot));
  group->pages[bpage->lru_slot] = dpage;
  dpage->lru_group = group;
  dpage->lru_slot = bpage->lru_slot;
  bpage->lru_group = nullptr;
}

/** Allocate and initialize one reusable LRU group. No LRU mutex is required.
Does NOT assign reuse_generation, since that counter
(LRU_group_next_reuse_generation) is protected by topology-X and this
function deliberately takes no lock. buf_lru_group_alloc() assigns it right
after calling this in its own reserve-empty branch; any other direct caller
(e.g. buf_LRU_drain_promote_queue()'s private staging group, PS-11141
Requirement 5) must assign it itself under topology-X before linking the
group into buf_pool->LRU -- buf_LRU_validate_instance() asserts
reuse_generation != 0 for every LINKED group. */
static buf_lru_group_t *buf_lru_group_create() {
  auto *group = ut::new_withkey<buf_lru_group_t>(UT_NEW_THIS_FILE_PSI_KEY);
  mutex_create(LATCH_ID_BUF_POOL_LRU_GROUP, &group->mutex);
  group->pages.fill(nullptr);
  group->n_pages = 0;
  group->occupied_slots = 0;
  group->old = false;
  group->state = buf_lru_group_state_t::RESERVE;
  group->cache_next = nullptr;
  group->n_maintenance_refs = 0;
  group->empty_candidate_pending.store(false, std::memory_order_relaxed);
  return group;
}

/** Obtains an empty LRU group (PS-11141 grouped LRU list), reusing one from
buf_pool->LRU_group_cache when available and only allocating when the reserve
is empty. Not yet linked into buf_pool->LRU.
@param[in,out]  buf_pool        buffer pool instance
@return an empty group */
static buf_lru_group_t *buf_lru_group_alloc(buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  buf_lru_group_t *group = buf_pool->LRU_group_cache;
  if (group != nullptr) {
    buf_pool->LRU_group_cache = group->cache_next;
    buf_pool->LRU_group_cache_len--;
    group->cache_next = nullptr;
    /* A reserved group was empty when it was cached and nothing touches an
    unlinked group, so its slots are all still null: only classification
    flags and the reuse generation need updating. */
    ut_ad(group->n_pages == 0);
    ut_ad(group->occupied_slots == 0);
    ut_ad(group->state == buf_lru_group_state_t::RESERVE);
    ut_ad(group->n_maintenance_refs == 0);
    ut_ad(!group->empty_candidate_pending.load(std::memory_order_relaxed));
    group->old = false;
  } else {
    group = buf_lru_group_create();
  }

  const uint64_t generation = buf_pool->LRU_group_next_reuse_generation++;
  ut_a(generation != 0);
  group->reuse_generation = generation;
  return group;
}

/** Destroy one empty, unlinked group. Must not hold topology-X.
STAGING is included because a drain's private staging group
(buf_LRU_drain_promote_queue(), PS-11141 Requirement 5) is destroyed this
same way when a chunk stages nothing: it is never put through
buf_lru_group_release() into the shared reserve/retired lists.
@param[in,out]  group group to destroy */
static void buf_lru_group_destroy(buf_lru_group_t *group) {
  ut_ad(group->n_pages == 0);
  ut_ad(group->occupied_slots == 0);
  ut_ad(group->state == buf_lru_group_state_t::RESERVE ||
        group->state == buf_lru_group_state_t::RETIRED ||
        group->state == buf_lru_group_state_t::STAGING);
  ut_ad(group->n_maintenance_refs == 0);
  ut_ad(!group->empty_candidate_pending.load(std::memory_order_relaxed));
  mutex_free(&group->mutex);
  ut::delete_(group);
}

/** Releases an empty LRU group into the bounded reserve, or retires it for
later destruction outside topology-X when the reserve is full. Performs
the LINKED -> RESERVE/RETIRED state transition; the caller must not have
already changed group->state.
@param[in,out]  buf_pool        buffer pool instance
@param[in,out]  group           empty group, already unlinked from
                                buf_pool->LRU via UT_LIST_REMOVE but still
                                marked LINKED */
static void buf_lru_group_release(buf_pool_t *buf_pool,
                                  buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(group->n_pages == 0);
  ut_ad(group->occupied_slots == 0);
  ut_ad(group->state == buf_lru_group_state_t::LINKED);
  ut_ad(group->n_maintenance_refs == 0);
  ut_ad(!group->empty_candidate_pending.load(std::memory_order_relaxed));

  if (buf_pool->LRU_group_cache_len < BUF_LRU_GROUP_RESERVE_MAX) {
    group->state = buf_lru_group_state_t::RESERVE;
    group->cache_next = buf_pool->LRU_group_cache;
    buf_pool->LRU_group_cache = group;
    buf_pool->LRU_group_cache_len++;
    return;
  }

  group->state = buf_lru_group_state_t::RETIRED;
  group->cache_next = buf_pool->LRU_group_retired;
  buf_pool->LRU_group_retired = group;
  buf_pool->LRU_group_retired_len++;
}

/** Steal the retired list under topology-X.
@param[in,out]  buf_pool buffer pool instance
@return stolen retired list head */
static buf_lru_group_t *buf_lru_group_steal_retired(buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  buf_lru_group_t *head = buf_pool->LRU_group_retired;
  buf_pool->LRU_group_retired = nullptr;
  buf_pool->LRU_group_retired_len = 0;
  return head;
}

/** Steal at most budget retired groups under topology-X.
@param[in,out] buf_pool buffer pool instance
@param[in] budget maximum groups to steal
@return stolen retired-list prefix */
static buf_lru_group_t *buf_lru_group_steal_retired(buf_pool_t *buf_pool,
                                                    size_t budget) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(budget > 0);

  buf_lru_group_t *head = buf_pool->LRU_group_retired;
  buf_lru_group_t *tail = head;
  size_t stolen = 0;
  while (tail != nullptr && ++stolen < budget) {
    tail = tail->cache_next;
  }
  if (tail == nullptr) {
    return buf_lru_group_steal_retired(buf_pool);
  }

  buf_pool->LRU_group_retired = tail->cache_next;
  tail->cache_next = nullptr;
  ut_a(buf_pool->LRU_group_retired_len >= stolen);
  buf_pool->LRU_group_retired_len -= stolen;
  return head;
}

/** Destroy a stolen retired list outside topology-X.
@param[in,out]  head stolen list head */
static void buf_lru_group_destroy_list(buf_lru_group_t *head) {
  while (head != nullptr) {
    buf_lru_group_t *next = head->cache_next;
    buf_lru_group_destroy(head);
    head = next;
  }
}

/** Steal reserved and retired lists under topology-X.
@param[in,out] buf_pool buffer pool instance
@param[out] reserved stolen reserve list
@param[out] retired stolen retired list */
static void buf_lru_group_steal_cache_lists(buf_pool_t *buf_pool,
                                            buf_lru_group_t **reserved,
                                            buf_lru_group_t **retired) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  *reserved = buf_pool->LRU_group_cache;
  buf_pool->LRU_group_cache = nullptr;
  buf_pool->LRU_group_cache_len = 0;
  *retired = buf_lru_group_steal_retired(buf_pool);
}

/** Frees every reserved and retired group. Called only at buffer pool
teardown after the topology latch has already been destroyed, so the
instance is quiescent.
@param[in,out]  buf_pool        buffer pool instance */
void buf_LRU_free_group_cache(buf_pool_t *buf_pool) {
  buf_lru_group_t *reserved = buf_pool->LRU_group_cache;
  buf_pool->LRU_group_cache = nullptr;
  buf_pool->LRU_group_cache_len = 0;

  buf_lru_group_t *retired = buf_pool->LRU_group_retired;
  buf_pool->LRU_group_retired = nullptr;
  buf_pool->LRU_group_retired_len = 0;

  buf_lru_group_destroy_list(reserved);
  buf_lru_group_destroy_list(retired);
}

/** Steal and destroy every reserved and retired group while the topology
latch is still live. Used by invalidation; concurrent stealers serialize on
topology-X so each group is destroyed at most once.
@param[in,out]  buf_pool        buffer pool instance */
void buf_LRU_empty_group_cache(buf_pool_t *buf_pool) {
  ut_ad(!buf_pool->LRU_topology_latch.owns_x());

  buf_pool->LRU_topology_latch.x_lock();
  buf_lru_group_t *reserved = nullptr;
  buf_lru_group_t *retired = nullptr;
  buf_lru_group_steal_cache_lists(buf_pool, &reserved, &retired);
  buf_pool->LRU_topology_latch.x_unlock();

  buf_lru_group_destroy_list(reserved);
  buf_lru_group_destroy_list(retired);
}

/** Reclaim retired groups and replenish the reserve outside topology-X.
The target and maximum form low/high watermarks: background work refills only
after the reserve falls below the target, while hot-path releases may retain
up to the higher maximum before retiring overflow. */
bool buf_LRU_maintain_group_cache(buf_pool_t *buf_pool, bool exhaustive) {
  ut_ad(!buf_pool->LRU_topology_latch.owns_x());

  const size_t destroy_budget = exhaustive ? std::numeric_limits<size_t>::max()
                                           : BUF_LRU_GROUP_DESTROY_BUDGET;
  const size_t create_budget =
      exhaustive ? BUF_LRU_GROUP_RESERVE_TARGET : BUF_LRU_GROUP_CREATE_BUDGET;

  const size_t empty_candidate_budget =
      exhaustive ? std::numeric_limits<size_t>::max()
                 : BUF_LRU_EMPTY_CANDIDATE_DRAIN_BUDGET;

  buf_pool->LRU_topology_latch.x_lock();
  bool empty_candidates_pending =
      buf_LRU_process_empty_candidates(buf_pool, empty_candidate_budget);
  buf_lru_group_t *retired =
      buf_pool->LRU_group_retired == nullptr
          ? nullptr
          : buf_lru_group_steal_retired(buf_pool, destroy_budget);
  const size_t reserve_len = buf_pool->LRU_group_cache_len;
  buf_pool->LRU_topology_latch.x_unlock();

  buf_lru_group_destroy_list(retired);

  if (reserve_len >= BUF_LRU_GROUP_RESERVE_TARGET) {
    buf_pool->LRU_topology_latch.x_lock();
    const bool pending =
        buf_pool->LRU_group_retired != nullptr || empty_candidates_pending;
    buf_pool->LRU_topology_latch.x_unlock();
    return pending;
  }

  const size_t created =
      std::min(BUF_LRU_GROUP_RESERVE_TARGET - reserve_len, create_budget);
  buf_lru_group_t *fresh = nullptr;
  for (size_t i = 0; i < created; ++i) {
    buf_lru_group_t *group = buf_lru_group_create();
    group->cache_next = fresh;
    fresh = group;
  }

  buf_pool->LRU_topology_latch.x_lock();
  while (fresh != nullptr &&
         buf_pool->LRU_group_cache_len < BUF_LRU_GROUP_RESERVE_TARGET) {
    buf_lru_group_t *group = fresh;
    fresh = fresh->cache_next;
    group->cache_next = buf_pool->LRU_group_cache;
    buf_pool->LRU_group_cache = group;
    buf_pool->LRU_group_cache_len++;
  }
  buf_pool->LRU_topology_latch.x_unlock();

  /* A concurrent release may have filled the reserve while creation ran. */
  buf_lru_group_destroy_list(fresh);

  buf_pool->LRU_topology_latch.x_lock();
  const bool pending =
      buf_pool->LRU_group_retired != nullptr ||
      buf_pool->LRU_group_cache_len < BUF_LRU_GROUP_RESERVE_TARGET ||
      empty_candidates_pending;
  buf_pool->LRU_topology_latch.x_unlock();
  return pending;
}

/** Result of detaching a page while holding topology-X. */
struct Detach_from_group_result {
  /** True if the page's group is not LINKED (it is mid-promotion in a
  drain's private staging group, PS-11141 Requirement 16): nothing was
  mutated, and group/group_now_empty/was_old below are meaningless. */
  bool skipped_staging;
  /** True if the group's page count reached zero. */
  bool group_now_empty;
  /** buf_page_is_old(bpage) as observed at the moment of detach. */
  bool was_old;
  /** The group the page was detached from. */
  buf_lru_group_t *group;
};

/** Vacate a page's slot in its current LRU group.

Under topology-X the topology latch alone excludes every other mutator, so
no group mutex is needed here. As of PS-11141 Requirement 8, the caller may
instead hold topology-S plus this page's block/zip mutex; in that case the
caller (buf_LRU_remove_block()) must also hold this specific group's mutex
across this call AND across its own subsequent handling of a newly-emptied
group (buf_LRU_group_became_empty()'s fill-pointer clearing and publish) --
releasing it any earlier would let a concurrent topology-S appender
(buf_LRU_try_append_fresh_S()) revalidate and append into the group in the
gap, silently un-emptying it while it is being published as a reclaim
candidate. This function itself neither takes nor releases that mutex; it
only relies on the caller already holding it when required.
@param[in,out]  bpage   control block, still linked into its group
@return see Detach_from_group_result */
static inline Detach_from_group_result buf_LRU_detach_from_group(
    buf_page_t *bpage) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(buf_pool->LRU_topology_latch.owns_x() ||
        mutex_own(&bpage->lru_group->mutex));
  buf_lru_group_t *group = bpage->lru_group;
  ut_a(group);

  if (group->state != buf_lru_group_state_t::LINKED) {
    DBUG_PRINT("ib_buf", ("skip reclassify of page " UINT32PF ":" UINT32PF
                          " -- its group is mid-promotion (state %u)",
                          bpage->id.space(), bpage->id.page_no(),
                          static_cast<unsigned>(group->state)));
    /* PS-11141 Requirement 16: the page is mid-promotion, sitting in a
    drain's private staging group under only LRU_drain_mutex plus
    topology-S/block/group protection -- topology-X alone does not make
    it safe to touch (the staging group is not linked into buf_pool->LRU,
    so nothing here stabilizes it against the drain's own concurrent S-mode
    move). Skip: the caller's requested reclassification (make old/young)
    is a heuristic, not a correctness requirement, so losing this one
    request to an in-flight promotion is benign -- the same class of
    benign skip as the was_io_fix_read() early return in
    buf_page_make_young_if_needed(). The eviction path can never reach
    this branch: buf_LRU_block_remove_hashed() already asserts
    buf_fix_count == 0 before calling buf_LRU_remove_block(), and every
    staged page stays fixed until the drain publishes it. */
    return {true, false, false, nullptr};
  }

  ut_ad(group->pages[bpage->lru_slot] == bpage);
  const bool was_old = bpage->old;
  ut_ad(was_old == group->old);
  ut_ad(group->occupied_slots & (uint32_t{1} << bpage->lru_slot));
  group->pages[bpage->lru_slot] = nullptr;
  group->occupied_slots &= ~(uint32_t{1} << bpage->lru_slot);
  group->n_pages--;
  const bool now_empty = (group->n_pages == 0);
  bpage->lru_group = nullptr;

  /* A sparse survivor may merge directly, while removing an empty group can
  make its former neighbors newly adjacent and mergeable. */
  buf_pool->LRU_compaction_pending.store(true, std::memory_order_relaxed);
  buf_pool->LRU_compaction_epoch.fetch_add(1, std::memory_order_relaxed);

  return {false, now_empty, was_old, group};
}

/** Increments buf_pool->LRU_n_pages by one page. Topology-X was the sole
mutator through PS-11141 Step 6; as of Step 7, buf_LRU_try_append_fresh_S()
also calls this under topology-S plus the destination group's mutex.
Relaxed ordering remains sufficient under S for the same reason it does
under X: the destination group's mutex, taken and revalidated by every
caller before this runs, is what actually orders concurrent updates -- the
atomic operation itself only needs to avoid a torn read/write.
@param[in,out]  buf_pool  buffer pool instance */
static inline void buf_LRU_n_pages_inc(buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  buf_pool->LRU_n_pages.fetch_add(1, std::memory_order_relaxed);
}

/** Decrements buf_pool->LRU_n_pages by one page. See buf_LRU_n_pages_inc().
@param[in,out]  buf_pool  buffer pool instance */
static inline void buf_LRU_n_pages_dec(buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  buf_pool->LRU_n_pages.fetch_sub(1, std::memory_order_relaxed);
}

/** Adds delta pages to buf_pool->LRU_old_len. See buf_LRU_n_pages_inc() for
the ordering rationale. Unlike LRU_n_pages, this one does have a real
topology-S caller as of PS-11141 Step 6: buf_LRU_stage_promote_page() moves
a page out of an old-classified source group into the (young) staging
group under S plus both groups' mutexes. Relaxed remains sufficient there
for the same reason it does under X: the source and destination group
mutexes serialize the two halves of any given move, so no reader can
observe a torn delta -- the group mutex is what supplies the ordering for
that specific update, not the atomic operation itself.
@param[in,out]  buf_pool  buffer pool instance
@param[in]      delta     number of pages to add */
static inline void buf_LRU_old_len_add(buf_pool_t *buf_pool, size_t delta) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  buf_pool->LRU_old_len.fetch_add(delta, std::memory_order_relaxed);
}

/** Increments buf_pool->LRU_old_len by one page. See buf_LRU_n_pages_inc().
@param[in,out]  buf_pool  buffer pool instance */
static inline void buf_LRU_old_len_inc(buf_pool_t *buf_pool) {
  buf_LRU_old_len_add(buf_pool, 1);
}

/** Decrements buf_pool->LRU_old_len by one page. See
buf_LRU_old_len_add() for why topology-S is sufficient here too.
@param[in,out]  buf_pool  buffer pool instance */
static inline void buf_LRU_old_len_dec(buf_pool_t *buf_pool) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  buf_pool->LRU_old_len.fetch_sub(1, std::memory_order_relaxed);
}

/** Reclaims an empty LRU group (PS-11141 grouped LRU list). If the group was
the LRU_old boundary, shifts the boundary to its predecessor group first
(mirroring the page-model's handling of "bpage == LRU_old"): the
predecessor is guaranteed to exist, because LRU_old is only allowed to
differ from the strict buf_pool->LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV target
by the group-level tolerance enforced in buf_LRU_old_adjust_len(). Clears
buf_pool->LRU_fill_group/LRU_young_fill_group if either pointed at this
group, adjusts the group hazard pointers, then unlinks and frees it.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  group     empty group (caller observes group->n_pages == 0
                          under topology-X); must not
                          be referenced again by the caller after this call */
static void buf_LRU_reclaim_empty_group(buf_pool_t *buf_pool,
                                        buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(group->n_pages == 0);

  if (group == buf_pool->LRU_old) {
    buf_lru_group_t *prev_group = UT_LIST_GET_PREV(LRU, group);
    ut_a(prev_group);

#ifdef UNIV_LRU_DEBUG
    ut_a(!prev_group->old);
#endif /* UNIV_LRU_DEBUG */
    prev_group->old = true;
    for (auto *p : prev_group->pages) {
      if (p != nullptr) {
        buf_page_set_old(p, true);
      }
    }
    buf_LRU_old_len_add(buf_pool, prev_group->n_pages);

    buf_pool->LRU_old = prev_group;
  }

  if (buf_pool->LRU_fill_group == group) {
    buf_pool->LRU_fill_group = nullptr;
  }
  if (buf_pool->LRU_young_fill_group == group) {
    buf_pool->LRU_young_fill_group = nullptr;
  }

  /* Important that we adjust the group hazard pointers before removing
  the (now-empty) group from the LRU list. */
  buf_LRU_adjust_group_hp(buf_pool, group);

  UT_LIST_REMOVE(buf_pool->LRU, group);
  buf_lru_group_release(buf_pool, group);
}

/** True if an active unlocked scan currently hazards this group. Defined
below; forward-declared here so the empty-candidate machinery (which
predates buf_lru_group_is_active()/buf_lru_group_has_live_hazard() in file
order) can use it. */
static bool buf_lru_group_has_live_hazard(const buf_pool_t *buf_pool,
                                          const buf_lru_group_t *group);

/** Attempts to enqueue an empty, LINKED, pending group onto
buf_pool->LRU_empty_candidates, taking the maintenance reference that keeps
it alive until buf_LRU_process_empty_candidates() consumes it. On overflow,
falls back to the persistent fallback scan instead (PS-11141 Requirement 9):
does not block or allocate.

Callable under topology-X (no group mutex needed -- X already excludes every
other mutator) or, as of PS-11141 Requirement 8, under topology-S plus the
group's own mutex (via buf_LRU_group_became_empty()): either way, the caller
is the only thread that can be publishing this specific group (only one
thread ever observes a given group's transition to empty), so
n_maintenance_refs stays a plain, non-atomic counter -- see its declaration.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  group     empty, LINKED group with empty_candidate_pending
                          already set by the caller */
static void buf_LRU_enqueue_empty_candidate(buf_pool_t *buf_pool,
                                            buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(group->n_pages == 0);
  ut_ad(group->state == buf_lru_group_state_t::LINKED);
  ut_ad(group->empty_candidate_pending.load(std::memory_order_relaxed));

  if (buf_pool->LRU_empty_candidates->try_push(group) ==
      ut::Bounded_mpsc_push_result::full) {
    /* Overflow: this group stays pending and LINKED, but untracked by the
    queue. The fallback scan below will find it via n_pages == 0 &&
    empty_candidate_pending, so no reference is needed for this path --
    there is no raw pointer sitting outside the natural buf_pool->LRU
    traversal that could go stale. */
    buf_pool->LRU_empty_scan_pending.store(true, std::memory_order_relaxed);
    return;
  }

  group->n_maintenance_refs++;
}

/** Publishes a newly-emptied, still-LINKED group as a reclaim candidate
instead of reclaiming it inline (PS-11141 Requirement 8/9). Idempotent: a
group already marked pending is not published twice.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  group     empty, LINKED group */
static void buf_LRU_publish_empty_candidate(buf_pool_t *buf_pool,
                                            buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(group->n_pages == 0);
  ut_ad(group->state == buf_lru_group_state_t::LINKED);

  if (group->empty_candidate_pending.exchange(true,
                                              std::memory_order_relaxed)) {
    return;
  }
  buf_LRU_enqueue_empty_candidate(buf_pool, group);
}

/** Re-validates a group pulled off buf_pool->LRU_empty_candidates or found
by the fallback scan, then reclaims it if still eligible (PS-11141
Requirement 8). The caller has already released any queue reference; this
function only clears empty_candidate_pending and hands off to
buf_LRU_reclaim_empty_group() when safe.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  group     candidate group; must not be referenced again by
                          the caller once this returns, since it may have
                          been unlinked and freed */
static void buf_LRU_try_reclaim_pending_group(buf_pool_t *buf_pool,
                                              buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  if (group->state != buf_lru_group_state_t::LINKED || group->n_pages != 0 ||
      group->n_maintenance_refs != 0 ||
      buf_lru_group_has_live_hazard(buf_pool, group)) {
    /* No longer eligible right now (or already reclaimed via another
    path); leave it pending so a later pass retries it. Only the hazard
    check can make an otherwise-empty LINKED group ineligible today, and
    per buf_LRU_reclaim_empty_group()'s own contract that can only be
    transient. */
    return;
  }

  group->empty_candidate_pending.store(false, std::memory_order_relaxed);
  buf_LRU_reclaim_empty_group(buf_pool, group);
}

/** Sweeps buf_pool->LRU for pending-but-unqueued empty groups left behind
by a buf_LRU_enqueue_empty_candidate() overflow (PS-11141 Requirement 9),
resuming from LRU_empty_scan_cursor across bounded passes.
@param[in,out]  buf_pool  buffer pool instance
@param[in]      budget    maximum groups to examine this call */
static void buf_LRU_scan_for_empty_groups(buf_pool_t *buf_pool, size_t budget) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  if (!buf_pool->LRU_empty_scan_pending.load(std::memory_order_relaxed)) {
    return;
  }

  buf_lru_group_t *group = buf_pool->LRU_empty_scan_cursor.get();
  if (group == nullptr) {
    group = UT_LIST_GET_LAST(buf_pool->LRU);
  }

  size_t examined = 0;
  while (group != nullptr && examined < budget) {
    /* Hazard the predecessor before touching group, mirroring the flush
    scan in buf_flush_LRU_list_batch(): group may be unlinked and freed
    below (via buf_LRU_try_reclaim_pending_group), so it must not be
    dereferenced again after that call. */
    buf_lru_group_t *const prev = UT_LIST_GET_PREV(LRU, group);
    buf_pool->LRU_empty_scan_cursor.set(prev);
    ++examined;

    if (group->n_pages == 0 &&
        group->empty_candidate_pending.load(std::memory_order_relaxed)) {
      buf_LRU_try_reclaim_pending_group(buf_pool, group);
    }

    group = buf_pool->LRU_empty_scan_cursor.get();
  }

  if (group == nullptr) {
    /* Reached the head: one full pass complete. */
    buf_pool->LRU_empty_scan_pending.store(false, std::memory_order_relaxed);
  }
}

/** Drains up to budget entries from buf_pool->LRU_empty_candidates,
reclaiming each if still eligible, then runs the fallback scan for any
overflow left behind (PS-11141 Requirement 8/9). Called periodically from
buf_LRU_maintain_group_cache() and exhaustively during invalidation.
@param[in,out]  buf_pool  buffer pool instance
@param[in]      budget    maximum candidates to drain from the queue this
                          call; the fallback scan gets the same budget
@return true if candidates or a pending scan remain */
bool buf_LRU_process_empty_candidates(buf_pool_t *buf_pool, size_t budget) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  size_t processed = 0;
  while (processed < budget) {
    auto popped = buf_pool->LRU_empty_candidates->try_pop();
    if (!popped.has_value()) {
      break;
    }
    buf_lru_group_t *group = *popped;
    ++processed;

    ut_a(group->n_maintenance_refs > 0);
    group->n_maintenance_refs--;
    buf_LRU_try_reclaim_pending_group(buf_pool, group);
  }

  buf_LRU_scan_for_empty_groups(buf_pool, budget);

  return !buf_pool->LRU_empty_candidates->empty() ||
         buf_pool->LRU_empty_scan_pending.load(std::memory_order_relaxed);
}

/** Decides whether a newly-emptied, still-LINKED group can be reclaimed
inline or must be deferred to buf_pool->LRU_empty_candidates (PS-11141
Requirement 8/9).

Under topology-X, this takes the inline path whenever
buf_lru_group_has_live_hazard() is false for the group (a live hazard only
ever targets a group's *neighbor*, precisely so the group under examination
stays freely reclaimable -- see buf_flush_LRU_list_batch()).
buf_lru_group_force_deferred_reclaim forces the deferred path for testing.

Under topology-S (PS-11141 Requirement 8, via buf_LRU_remove_block()'s
topology-S fast path), inline reclaim is never attempted: unlinking a group
from buf_pool->LRU mutates the topology itself, which requires topology-X.
Every group observed empty under topology-S is therefore unconditionally
published for a later topology-X maintenance batch to revalidate and
reclaim -- the hazard check that lets the topology-X path skip the queue
would be wasted work here, since that path is unreachable anyway.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  group     empty group; caller holds this group's mutex when
                          holding topology-S (see buf_LRU_remove_block()),
                          or topology-X alone otherwise; must not be
                          referenced again by the caller after this call */
static void buf_LRU_group_became_empty(buf_pool_t *buf_pool,
                                       buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(buf_pool->LRU_topology_latch.owns_x() || mutex_own(&group->mutex));
  ut_ad(group->n_pages == 0);

  /* An empty group must never remain a fill target, whether it is
  reclaimed inline below or deferred: buf_LRU_append_to_{young,old}_fill_
  group() only check n_pages/old before appending into buf_pool->LRU_
  {young_,}fill_group, so a pending-but-still-linked group left as a fill
  pointer would silently un-empty itself while empty_candidate_pending
  stayed set -- caught by buf_LRU_validate_instance() the hard way once
  already. buf_LRU_reclaim_empty_group() repeats this clearing on the
  inline path; that is a harmless no-op once already done here. */
  if (buf_pool->LRU_fill_group == group) {
    buf_pool->LRU_fill_group = nullptr;
  }
  if (buf_pool->LRU_young_fill_group == group) {
    buf_pool->LRU_young_fill_group = nullptr;
  }

  bool defer = true;
  if (buf_pool->LRU_topology_latch.owns_x()) {
    defer = buf_lru_group_has_live_hazard(buf_pool, group);
    DBUG_EXECUTE_IF("buf_lru_group_force_deferred_reclaim", defer = true;);
  }

  if (defer) {
    buf_LRU_publish_empty_candidate(buf_pool, group);
    return;
  }

  buf_LRU_reclaim_empty_group(buf_pool, group);
}

/** Complete block removal after its group slot was vacated.

Callable under topology-X alone, or (PS-11141 Requirement 8) under
topology-S plus this group's mutex, held by the caller across this entire
call (buf_LRU_remove_block()) -- required so buf_LRU_group_became_empty()'s
fill-pointer clearing and publish, below, cannot race a concurrent
topology-S appender revalidating the same group. adjust_old must be false
under topology-S: see buf_LRU_remove_block()'s comment for why boundary
convergence is deliberately deferred there.
@param[in]      bpage           the same page passed to the preceding
                                buf_LRU_detach_from_group() call
@param[in]      group           bpage->lru_group as observed before that
                                call (bpage->lru_group is nullptr by now)
@param[in]      was_old         buf_page_is_old(bpage) as observed before
                                that call
@param[in]      group_now_empty return value of that call
@param[in]      adjust_old      whether to run the LRU_old boundary
                                maintenance below; must be false whenever
                                the caller holds topology-S */
static inline void buf_LRU_remove_block_finish(buf_page_t *bpage,
                                               buf_lru_group_t *group,
                                               bool was_old,
                                               bool group_now_empty,
                                               bool adjust_old) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(adjust_old ? buf_pool->LRU_topology_latch.owns_x() : true);

  /* Cleared under topology-X after the slot and back-pointer update. */
  ut_d(bpage->in_LRU_list = false);

  buf_pool->stat.LRU_bytes -= bpage->size.physical();
  buf_LRU_n_pages_dec(buf_pool);

  buf_unzip_LRU_remove_block_if_needed(bpage);

  if (was_old) {
    buf_LRU_old_len_dec(buf_pool);
  }

  if (group_now_empty) {
    buf_LRU_group_became_empty(buf_pool, group);
  }

  if (!adjust_old) {
    /* PS-11141 Requirement 8/9: no assertion on LRU_n_pages here. The
    topology-S eligibility check that led to adjust_old == false
    (buf_LRU_n_pages > BUF_LRU_OLD_MIN_LEN, checked by the caller before
    ever reaching this function) is a heuristic snapshot, not a reserved
    budget: multiple concurrent topology-S evictors can each pass it and
    then decrement concurrently, collectively taking the count below the
    threshold even though no single one observed a violation. That is
    harmless here -- boundary convergence is already deferred whenever
    adjust_old is false, exactly like the topology-S insertion fast path
    (buf_LRU_try_append_fresh_S()), and the same natural convergence points
    (group reclaim, the next topology-X path, buf_LRU_validate_instance()'s
    forced adjustment) apply regardless of which side of the threshold the
    count transiently sits on. */
    return;
  }

  /* If the LRU list is so short that LRU_old is not defined,
  clear the "old" flags and return */
  if (buf_pool->LRU_n_pages < BUF_LRU_OLD_MIN_LEN) {
    for (auto *g : buf_pool->LRU) {
      /* This loop temporarily violates the assertions of
      buf_page_set_old(). */
      g->old = false;
      for (auto *p : g->pages) {
        if (p != nullptr) {
          p->old.store(false, std::memory_order_relaxed);
        }
      }
    }

    buf_pool->LRU_old = nullptr;
    buf_pool->LRU_old_len = 0;

    return;
  }

  ut_ad(buf_pool->LRU_old);

  /* Adjust the length of the old block list if necessary */
  buf_LRU_old_adjust_len(buf_pool);
}

/** Removes a block from the LRU list (PS-11141 grouped LRU list): vacates
its slot in its group, and if that empties the group, unlinks and frees the
group under topology-X.

As of PS-11141 Requirement 8, the caller may instead hold topology-S plus
this page's block/zip mutex -- buf_LRU_try_evict_tail_identity() does so
for the common case (a plain, uncompressed FILE_PAGE, once the LRU is long
enough that boundary maintenance may be deferred). This function then takes
and holds the page's group mutex across both the detach and, if the group
empties, buf_LRU_group_became_empty()'s fill-pointer clearing and publish
-- releasing it any earlier would let a concurrent topology-S appender
(buf_LRU_try_append_fresh_S()) revalidate and append into the group in the
gap, silently un-emptying it while it is being published as a reclaim
candidate. adjust_old is passed as false in that case: deliberately does
not call buf_LRU_old_adjust_len(), for the same reason
buf_LRU_try_append_fresh_S() does not on the insertion side -- letting every
topology-S eviction also probe/adjust the old/young boundary would
reintroduce the pool-wide serialization this step exists to remove.
Convergence instead happens where it already does on the topology-X-only
paths: buf_LRU_reclaim_empty_group() shifts the boundary by one group
whenever the reclaimed group is the boundary group, and
buf_LRU_validate_instance() forces one bounded adjustment before its
ratio-tolerance assertion.
@param[in]      bpage   control block
@return false if the page is mid-promotion in a drain's private staging
        group and nothing was mutated (PS-11141 Requirement 16); true if
        the page was removed */
static inline bool buf_LRU_remove_block(buf_page_t *bpage) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);

  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());

  ut_a(buf_page_in_file(bpage));

  ut_ad(bpage->in_LRU_list);

  buf_lru_group_t *group = bpage->lru_group;
  ut_a(group);

  const bool under_s = buf_pool->LRU_topology_latch.owns_s();
  if (under_s) {
    mutex_enter(&group->mutex);
  }

  const auto detached = buf_LRU_detach_from_group(bpage);
  if (detached.skipped_staging) {
    if (under_s) {
      mutex_exit(&group->mutex);
    }
    return false;
  }

  buf_LRU_remove_block_finish(bpage, group, detached.was_old,
                              detached.group_now_empty, !under_s);

  if (under_s) {
    mutex_exit(&group->mutex);
  }
  return true;
}

/** Adds a block to the LRU list of decompressed zip pages.
@param[in]      block   control block
@param[in]      old     true if should be put to the end of the list,
                        else put to the start */
void buf_unzip_LRU_add_block(buf_block_t *block, bool old) {
  buf_pool_t *buf_pool = buf_pool_from_block(block);

  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  ut_a(buf_page_belongs_to_unzip_LRU(&block->page));

  ut_ad(!block->in_unzip_LRU_list);
  ut_d(block->in_unzip_LRU_list = true);

  if (old) {
    UT_LIST_ADD_LAST(buf_pool->unzip_LRU, block);
  } else {
    UT_LIST_ADD_FIRST(buf_pool->unzip_LRU, block);
  }
}

/** Finds an empty slot in constant time. The group must not already be full.
@param[in]      group   group protected by topology-X
@return index of an empty (null) slot */
static inline uint32_t buf_lru_group_find_free_slot(
    const buf_lru_group_t *group) {
  static_assert(BUF_LRU_GROUP_SIZE == sizeof(group->occupied_slots) * 8);
  ut_a(group->occupied_slots != UINT32_MAX);
  const uint32_t slot = std::countr_zero(~group->occupied_slots);
  ut_ad(group->pages[slot] == nullptr);
  return slot;
}

/** Appends a page to a non-full group.
@param[in,out] group group protected by topology-X
@param[in,out] bpage page not currently belonging to a group */
static inline void buf_lru_group_append_page(buf_lru_group_t *group,
                                             buf_page_t *bpage) {
  ut_ad(bpage->lru_group == nullptr);

  const uint32_t slot = buf_lru_group_find_free_slot(group);
  group->pages[slot] = bpage;
  group->occupied_slots |= uint32_t{1} << slot;
  group->n_pages++;
  bpage->lru_group = group;
  bpage->lru_slot = slot;
}

/** Appends bpage to buf_pool->LRU_young_fill_group (the young/MRU-side fill
group), creating and linking a new group at the LRU head first if the
current one is null or full (PS-11141 grouped LRU list). Used both by the
immediate buf_LRU_make_block_young() path and, once per drained page, by
buf_LRU_drain_promote_queue(), so the drain naturally produces
ceil(N/BUF_LRU_GROUP_SIZE) groups without special-casing.
Requires buf_pool->LRU_topology_latch. Does not set bpage->old or
freed_page_clock; the caller does.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  bpage     page to append; must not already belong to a group */
static void buf_LRU_append_to_young_fill_group(buf_pool_t *buf_pool,
                                               buf_page_t *bpage) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(bpage->lru_group == nullptr);

  buf_lru_group_t *group = buf_pool->LRU_young_fill_group;
  if (group != nullptr) {
    if (group->n_pages < BUF_LRU_GROUP_SIZE && !group->old) {
      buf_lru_group_append_page(group, bpage);
      buf_LRU_n_pages_inc(buf_pool);
      return;
    }
  }

  /* A full or reclassified fill group is abandoned; start a fresh group. */
  group = buf_lru_group_alloc(buf_pool);
  UT_LIST_ADD_FIRST(buf_pool->LRU, group);
  group->state = buf_lru_group_state_t::LINKED;
  buf_pool->LRU_young_fill_group = group;
  buf_lru_group_append_page(group, bpage);

  buf_LRU_n_pages_inc(buf_pool);
}

/** Appends bpage to buf_pool->LRU_fill_group (the old-side fill group),
creating and linking a new group as LRU_old's immediate group-successor
first if the current one is null or full (PS-11141 grouped LRU list). Used
both by fresh page-read insertion (via buf_LRU_add_block_low()) and by
buf_LRU_make_block_old(). Requires buf_pool->LRU_topology_latch and, by
contract, that buf_pool->LRU_old is already defined (callers only reach
this once the LRU is long enough -- see buf_LRU_add_block_low()); the
LRU_old == nullptr fallback below is defensive only. Increments
buf_pool->LRU_old_len; does not set bpage->old, which the caller sets to
match the group's old-ness.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  bpage     page to append; must not already belong to a group */
static void buf_LRU_append_to_old_fill_group(buf_pool_t *buf_pool,
                                             buf_page_t *bpage) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(bpage->lru_group == nullptr);

  buf_lru_group_t *group = buf_pool->LRU_fill_group;
  if (group != nullptr) {
    if (group->n_pages < BUF_LRU_GROUP_SIZE && group->old) {
      buf_lru_group_append_page(group, bpage);
      buf_LRU_n_pages_inc(buf_pool);
      buf_LRU_old_len_inc(buf_pool);
      return;
    }
  }

  /* A full or reclassified fill group is abandoned; start a fresh group. */
  group = buf_lru_group_alloc(buf_pool);
  group->old = true;
  if (buf_pool->LRU_old != nullptr) {
    UT_LIST_INSERT_AFTER(buf_pool->LRU, buf_pool->LRU_old, group);
  } else {
    /* Defensive fallback; not reachable via buf_LRU_add_block_low(),
    whose length check ensures LRU_old is already defined here. */
    UT_LIST_ADD_FIRST(buf_pool->LRU, group);
  }
  group->state = buf_lru_group_state_t::LINKED;
  buf_pool->LRU_fill_group = group;
  buf_lru_group_append_page(group, bpage);

  buf_LRU_n_pages_inc(buf_pool);
  buf_LRU_old_len_inc(buf_pool);
}

/** Adds a block to the LRU list. Please make sure that the page_size is
already set when invoking the function, so that we can get correct
page_size from the buffer page when adding a block into LRU
(PS-11141 grouped LRU list).
@param[in]      bpage   control block
@param[in]      old     true if should be put to the old blocks in the LRU list,
                        else put to the start; if the LRU list is very short,
                        the block is added to the start, regardless of this
                        parameter */
static inline void buf_LRU_add_block_low(buf_page_t *bpage, bool old,
                                         bool adjust_old = true) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);

  ut_ad(buf_pool->LRU_topology_latch.owns_x());

  ut_a(buf_page_in_file(bpage));
  ut_ad(!bpage->in_LRU_list);

  if (!old || (buf_pool->LRU_n_pages < BUF_LRU_OLD_MIN_LEN)) {
    buf_LRU_append_to_young_fill_group(buf_pool, bpage);

    bpage->freed_page_clock.store(
        buf_pool->freed_page_clock.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  } else {
#ifdef UNIV_LRU_DEBUG
    /* buf_pool->LRU_old must be the first group in the LRU list whose
    "old" flag is set. */
    ut_a(buf_pool->LRU_old->old);
    ut_a(!UT_LIST_GET_PREV(LRU, buf_pool->LRU_old) ||
         !UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)->old);
    ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old) ||
         UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */
    buf_LRU_append_to_old_fill_group(buf_pool, bpage);
  }

  ut_d(bpage->in_LRU_list = true);

  incr_LRU_size_in_bytes(bpage, buf_pool);

  if (!adjust_old) {
    ut_ad(buf_pool->LRU_n_pages > BUF_LRU_OLD_MIN_LEN);
    ut_ad(buf_pool->LRU_old);
    buf_page_set_old(bpage, old);
  } else if (buf_pool->LRU_n_pages > BUF_LRU_OLD_MIN_LEN) {
    ut_ad(buf_pool->LRU_old);

    /* Adjust the length of the old block list if necessary */

    buf_page_set_old(bpage, old);
    buf_LRU_old_adjust_len(buf_pool);

  } else if (buf_pool->LRU_n_pages == BUF_LRU_OLD_MIN_LEN) {
    /* The LRU list is now long enough for LRU_old to become
    defined: init it */

    buf_LRU_old_init(buf_pool);
  } else {
    buf_page_set_old(bpage, buf_pool->LRU_old != nullptr);
  }

  /* If this is a zipped block with decompressed frame as well
  then put it on the unzip_LRU list */
  if (buf_page_belongs_to_unzip_LRU(bpage)) {
    buf_unzip_LRU_add_block((buf_block_t *)bpage, old);
  }
}

/** Adds a block to the LRU list. Please make sure that the page_size is
 already set when invoking the function, so that we can get correct
 page_size from the buffer page when adding a block into LRU */
void buf_LRU_add_block(buf_page_t *bpage, /*!< in: control block */
                       bool old) /*!< in: true if should be put to the old
                                  blocks in the LRU list, else put to the start;
                                  if the LRU list is very short, the block is
                                  added to the start, regardless of this
                                  parameter */
{
  buf_LRU_add_block_low(bpage, old);
}

/** Locks two distinct group mutexes in a deterministic order (by pointer
address) to avoid an AB-BA deadlock on any path that must hold both at once
(PS-11141 Requirement 2: "multiple group locks must use one deterministic
order"). */
static void buf_lru_group_pair_lock(buf_lru_group_t *a, buf_lru_group_t *b) {
  ut_ad(a != b);
  if (a < b) {
    mutex_enter(&a->mutex);
    mutex_enter(&b->mutex);
  } else {
    mutex_enter(&b->mutex);
    mutex_enter(&a->mutex);
  }
}

/** Unlocks a pair locked by buf_lru_group_pair_lock(). Order does not
matter for release. */
static void buf_lru_group_pair_unlock(buf_lru_group_t *a, buf_lru_group_t *b) {
  mutex_exit(&a->mutex);
  mutex_exit(&b->mutex);
}

/** PS-11141 Requirement 7 topology-S fast path for buf_LRU_add_fresh_page():
attempts to append bpage into the current young or old fill group under
topology-S plus that group's mutex, without ever taking topology-X.

Bails (returns false) to the topology-X fallback -- which is simply
buf_LRU_add_block_low(), unchanged -- in every case that requires X:
  - warm-up, i.e. buf_pool->LRU_n_pages has not yet passed
    BUF_LRU_OLD_MIN_LEN. Crossing that threshold is a one-time, X-only
    structural transition (Requirement 12; buf_LRU_old_init()) and is rare
    enough that carving it out of the fast path costs nothing measurable.
  - bpage belongs to unzip_LRU. That list is still protected solely by
    topology-X (Requirement 11's dedicated unzip_LRU latch is not yet
    implemented); compressed-page territory is deliberately deferred
    throughout this effort (see Step 5).
  - no usable fill group exists: missing, full, wrong classification, or
    concurrently replaced since being read (revalidated under the group's
    own mutex, mirroring buf_LRU_stage_promote_page()'s authoritative
    re-check).
Deliberately does not call buf_LRU_old_adjust_len(): see the comment on
buf_LRU_add_fresh_page() for why the fast path leaves boundary convergence
to the natural fill-group-rollover cadence instead of running it on every
insertion.
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  bpage     control block, not yet in the LRU list
@param[in]      old       true to place among old blocks
@return true if bpage was appended; false if the caller must fall back to
        topology-X */
static bool buf_LRU_try_append_fresh_S(buf_pool_t *buf_pool, buf_page_t *bpage,
                                       bool old) {
  ut_a(buf_page_in_file(bpage));
  ut_ad(!bpage->in_LRU_list);

  if (buf_pool->LRU_n_pages.load(std::memory_order_relaxed) <=
          BUF_LRU_OLD_MIN_LEN ||
      buf_page_belongs_to_unzip_LRU(bpage)) {
    return false;
  }

  std::atomic<buf_lru_group_t *> &fill_ptr =
      old ? buf_pool->LRU_fill_group : buf_pool->LRU_young_fill_group;

  buf_pool->LRU_topology_latch.s_lock();

  buf_lru_group_t *group = fill_ptr.load(std::memory_order_relaxed);
  if (group == nullptr) {
    buf_pool->LRU_topology_latch.s_unlock();
    return false;
  }

  mutex_enter(&group->mutex);

  const bool usable = group->state == buf_lru_group_state_t::LINKED &&
                      group->n_pages < BUF_LRU_GROUP_SIZE &&
                      group->old == old &&
                      fill_ptr.load(std::memory_order_relaxed) == group;
  if (!usable) {
    mutex_exit(&group->mutex);
    buf_pool->LRU_topology_latch.s_unlock();
    return false;
  }

  /* Classification before publish (Requirement 10). */
  bpage->old.store(old, std::memory_order_relaxed);
  if (!old) {
    bpage->freed_page_clock.store(
        buf_pool->freed_page_clock.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  }

  buf_lru_group_append_page(group, bpage);
  ut_d(bpage->in_LRU_list = true);
  incr_LRU_size_in_bytes(bpage, buf_pool);
  buf_LRU_n_pages_inc(buf_pool);
  if (old) {
    buf_LRU_old_len_inc(buf_pool);
  }

  mutex_exit(&group->mutex);
  buf_pool->LRU_topology_latch.s_unlock();
  return true;
}

/** Adds a freshly-read or freshly-created block to the LRU list (PS-11141
Requirement 7). Self-locking: the caller must hold neither topology mode on
entry. Tries the topology-S fast path above first; falls back to
topology-X plus the unchanged buf_LRU_add_block_low() otherwise. Replaces
the callers in buf_page_init_for_read() that used to wrap a call to
buf_LRU_add_block() in their own topology-X acquisition -- that wrap is now
internal to this function and only taken when actually needed.

The fast path skips buf_LRU_old_adjust_len(): unlike topology-X, which
serializes every insertion pool-wide, topology-S lets concurrent inserters
each append to their own group-mutex-protected fill group, so making every
one of them also probe/adjust the old/young boundary would put back most of
the contention this step exists to remove. Convergence instead rides the
natural rollover cadence: a fill group holds at most BUF_LRU_GROUP_SIZE
pages, so it fills and rolls over (falling back to topology-X, which does
call buf_LRU_old_adjust_len() via buf_LRU_add_block_low(), unchanged) at
least once every BUF_LRU_GROUP_SIZE insertions on each side -- the same
granularity at which the boundary already moves. buf_LRU_validate_instance()
additionally forces one bounded adjustment before its ratio-tolerance
assertion, so a quiet period between rollovers cannot trip it (PS-11141
Requirement 10: "reschedules convergence without a self-wake loop").
@param[in,out]  bpage   control block, not yet in the LRU list
@param[in]      old     true to place among old blocks; if the LRU list is
                        very short, the block is added to the start
                        regardless */
void buf_LRU_add_fresh_page(buf_page_t *bpage, bool old) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());

  if (buf_LRU_try_append_fresh_S(buf_pool, bpage, old)) {
    return;
  }

  buf_pool->LRU_topology_latch.x_lock();
  buf_LRU_add_block_low(bpage, old);
  buf_pool->LRU_topology_latch.x_unlock();
}

/** PS-11141 Requirement 7 topology-S fast path for buf_LRU_make_block_young()
and buf_LRU_make_block_old(): moves bpage from its current group into the
young or old fill group under topology-S plus a deterministically-ordered
pair of group mutexes, without ever taking topology-X.

Bails (returns false) to the topology-X fallback in every case Step 8 (not
this step) is responsible for making S-safe, or that Requirement 11's
still-missing dedicated unzip_LRU latch would otherwise be needed for:
  - bpage's group is not LINKED (Requirement 16: mid-promotion in a drain's
    private staging group).
  - the move would empty the source group. Publishing an empty-linked-group
    reclaim candidate (buf_LRU_publish_empty_candidate()) still requires
    topology-X (Requirement 8/9 territory); this fast path never empties a
    group, full stop.
  - bpage belongs to unzip_LRU (see buf_LRU_try_append_fresh_S()).
  - no destination fill group exists, or revalidation under the pair-locked
    mutexes finds it stale/full/reclassified, or finds the source no longer
    matches (a concurrent detach raced us between reading bpage->lru_group
    and locking it).
Destination capacity is confirmed before the source is touched in any way,
so a page is never left detached while this function decides whether it
can complete the move (Requirement 7's "a page is never detached while
obtaining one").
@param[in,out]  bpage       control block, currently linked into a group
@param[in]      want_old    true to reclassify old (make-old), false to
                            reclassify young (make-young)
@param[out]     was_old     set to bpage's old classification as observed
                            before the move, iff this returns true
@return true if the move completed under topology-S; false if the caller
        must fall back to topology-X */
static bool buf_LRU_try_reclassify_S(buf_page_t *bpage, bool want_old,
                                     bool *was_old) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());

  if (buf_page_belongs_to_unzip_LRU(bpage)) {
    return false;
  }

  std::atomic<buf_lru_group_t *> &fill_ptr =
      want_old ? buf_pool->LRU_fill_group : buf_pool->LRU_young_fill_group;

  buf_pool->LRU_topology_latch.s_lock();

  BPageMutex *block_mutex = buf_page_get_mutex(bpage);
  mutex_enter(block_mutex);

  /* Only bpage->lru_group itself (a page-level field, synchronized by
  block_mutex -- every writer holds block_mutex throughout its critical
  section, mirroring buf_LRU_stage_promote_page()) is safe to read here.
  source->state/n_pages/pages[] are group-level fields synchronized by
  source->mutex, not by block_mutex or topology-S alone, so they must not
  be read before source is pair-locked below -- a concurrent S-mode mover
  (e.g. another buf_LRU_stage_promote_page() call) could be mutating them
  right now under only source->mutex. */
  buf_lru_group_t *source = bpage->lru_group;
  ut_a(source);

  buf_lru_group_t *dest = fill_ptr.load(std::memory_order_relaxed);
  if (dest == nullptr || source == dest) {
    /* No destination yet, or bpage is already there: every member of a
    group shares its "old" classification, so want_old already holds and
    there is nothing to move (the grouped design's coarser MRU-positioning
    precision, already accepted elsewhere -- see
    buf_LRU_stage_promote_page()). Also sidesteps pair-locking a group
    against itself, which buf_lru_group_pair_lock() disallows. */
    mutex_exit(block_mutex);
    buf_pool->LRU_topology_latch.s_unlock();
    return false;
  }

  buf_lru_group_pair_lock(source, dest);

  const bool usable =
      source->state == buf_lru_group_state_t::LINKED &&  // Requirement 16
      source->pages[bpage->lru_slot] == bpage &&
      source->n_pages > 1 &&  // never empty source -- see header comment
      dest->state == buf_lru_group_state_t::LINKED &&
      dest->n_pages < BUF_LRU_GROUP_SIZE &&
      /* A fill pointer's target can be reclassified out from under it by
      buf_LRU_old_adjust_len() (X-only, does not clear/repoint the fill
      pointers): if the LRU is short enough that the boundary sits next to
      a fill group, a boundary move can flip that very group's "old" flag
      without anyone updating LRU_fill_group/LRU_young_fill_group to
      match. buf_LRU_append_to_old_fill_group()/_young_fill_group() guard
      against this exact drift (their own "&& !group->old"/"&& group->old"
      checks); this is the same guard for the reclassify path. */
      dest->old == want_old && fill_ptr.load(std::memory_order_relaxed) == dest;

  if (!usable) {
    buf_lru_group_pair_unlock(source, dest);
    mutex_exit(block_mutex);
    buf_pool->LRU_topology_latch.s_unlock();
    return false;
  }

  *was_old = source->old;

  source->pages[bpage->lru_slot] = nullptr;
  source->occupied_slots &= ~(uint32_t{1} << bpage->lru_slot);
  source->n_pages--;
  bpage->lru_group = nullptr;

  /* Classification before publish (Requirement 10). */
  bpage->old.store(want_old, std::memory_order_relaxed);
  if (!want_old) {
    bpage->freed_page_clock.store(
        buf_pool->freed_page_clock.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  }

  buf_lru_group_append_page(dest, bpage);

  if (*was_old && !want_old) {
    buf_LRU_old_len_dec(buf_pool);
  } else if (!*was_old && want_old) {
    buf_LRU_old_len_inc(buf_pool);
  }

  buf_lru_group_pair_unlock(source, dest);
  mutex_exit(block_mutex);
  buf_pool->LRU_topology_latch.s_unlock();
  return true;
}

/** Moves a block to the start of the LRU list. Self-locking: the caller
must hold neither topology mode on entry (PS-11141 Requirement 7 replaced
this function's former contract of requiring the caller to already hold
topology-X).
@param[in]      bpage   control block */
void buf_LRU_make_block_young(buf_page_t *bpage) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());

  bool was_old = false;
  if (buf_LRU_try_reclassify_S(bpage, false, &was_old)) {
    if (was_old) {
      buf_pool->stat.n_pages_made_young++;
    }
    return;
  }

  buf_pool->LRU_topology_latch.x_lock();

  was_old = bpage->old;

  /* PS-11141 Requirement 16: a false return means bpage is mid-promotion
  in a drain's private staging group; nothing was mutated, so there is
  nothing left to do (see buf_LRU_detach_from_group()'s comment). */
  if (!buf_LRU_remove_block(bpage)) {
    buf_pool->LRU_topology_latch.x_unlock();
    return;
  }

  if (was_old) {
    buf_pool->stat.n_pages_made_young++;
  }

  buf_LRU_add_block_low(bpage, false);
  buf_pool->LRU_topology_latch.x_unlock();
}

/** Moves a block to the end of the LRU list. Self-locking: see
buf_LRU_make_block_young()'s comment.
@param[in]      bpage   control block */
void buf_LRU_make_block_old(buf_page_t *bpage) {
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());

  bool was_old = false;
  if (buf_LRU_try_reclassify_S(bpage, true, &was_old)) {
    return;
  }

  buf_pool->LRU_topology_latch.x_lock();

  /* See buf_LRU_make_block_young()'s comment on the false case. */
  if (!buf_LRU_remove_block(bpage)) {
    buf_pool->LRU_topology_latch.x_unlock();
    return;
  }

  buf_LRU_add_block_low(bpage, true);
  buf_pool->LRU_topology_latch.x_unlock();
}

/** Resolve and temporarily pin one deferred make-young identity.
@param[in,out] buf_pool buffer pool instance
@param[in] identity page residency requested by a producer
@return matching pinned descriptor, or nullptr */
static buf_page_t *buf_LRU_pin_promote_identity(
    buf_pool_t *buf_pool, const buf_lru_promote_t &identity) {
  rw_lock_t *hash_lock;
  buf_page_t *bpage =
      buf_page_hash_get_s_locked(buf_pool, identity.page_id, &hash_lock);

  if (bpage == nullptr) {
    return nullptr;
  }

  if (buf_pool_watch_is_sentinel(buf_pool, bpage) ||
      bpage->residency_generation != identity.residency_generation ||
      !buf_page_in_file(bpage)) {
    rw_lock_s_unlock(hash_lock);
    return nullptr;
  }

  /* The hash latch stabilizes the descriptor until this temporary consumer
  pin is installed. The pin then bridges the required hash-to-LRU latch-order
  transition without extending descriptor lifetime from the producer. */
  buf_block_fix(bpage);
  rw_lock_s_unlock(hash_lock);
  return bpage;
}

/** Result of one buf_LRU_stage_promote_page() attempt. */
struct Stage_promote_result {
  /** True if the page was moved into the staging group. */
  bool staged;
  /** The page's former group, if staged and it became empty as a result;
  nullptr otherwise. Not yet published to buf_pool->LRU_empty_candidates --
  the caller collects these locally and hands them to
  buf_LRU_group_became_empty() during the batch's single topology-X
  phase (PS-11141 Requirement 6). */
  buf_lru_group_t *emptied_source;
};

/** Attempts to move one pinned page from its current LRU group into the
drain's private staging group under topology-S (PS-11141 Requirement 5) --
no topology-X is taken. Acquisition order is topology-S, then the page's
block mutex, then the two groups' mutexes (buf_lru_group_pair_lock()),
matching SYNC_BUF_LRU_LIST > SYNC_BUF_BLOCK > SYNC_BUF_LRU_GROUP in
sync0types.h.

Re-validates the same identity/liveness predicates the legacy X-mode drain
used, plus the slot back-pointer, since time has passed since the page
was pinned under the page-hash latch in buf_LRU_pin_promote_identity():
the checks are duplicated once under only the block mutex (cheap,
filters obviously-stale pins) and once more under the block and group
mutexes together (authoritative, immediately before mutating).
@param[in,out]  buf_pool  buffer pool instance
@param[in,out]  staging   drain-private staging group; must have a free
                          slot
@param[in]      identity  producer-requested identity
@param[in,out]  bpage     pinned candidate; the caller keeps owning the pin
                          regardless of outcome
@return see Stage_promote_result */
static Stage_promote_result buf_LRU_stage_promote_page(
    buf_pool_t *buf_pool, buf_lru_group_t *staging,
    const buf_lru_promote_t &identity, buf_page_t *bpage) {
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(staging->n_pages < BUF_LRU_GROUP_SIZE);

  Stage_promote_result result{false, nullptr};

  buf_pool->LRU_topology_latch.s_lock();

  BPageMutex *block_mutex = buf_page_get_mutex(bpage);
  mutex_enter(block_mutex);

  buf_lru_group_t *source = bpage->lru_group;
  /* buf_page_peek_if_too_old() has no X-only precondition: it reads
  buf_pool-wide heuristics (freed_page_clock, LRU_old_ratio) and bpage->old,
  and its buf_page_peek_if_young() call asserts buf_fix_count > 0, which the
  caller's pin on bpage already satisfies. It bumps
  stat.n_pages_not_made_young non-atomically; a lost update under topology-S
  is the same benign approximation every other unsynchronized buf_pool
  stat counter already accepts. */
  /* Test-only: paired with the same-named hook in
  buf_page_make_young_if_needed() so a test can force real pages through
  the enqueue-drain-stage pipeline without needing to naturally trigger
  buf_page_peek_if_too_old()'s buffer-pool-aging heuristics (freed_page_clock
  drift, old_blocks_time). No effect unless the debug flag is set. */
  bool skip_too_old_recheck_for_test = false;
  DBUG_EXECUTE_IF("buf_lru_force_enqueue_promote",
                  skip_too_old_recheck_for_test = true;);
  if (bpage->id == identity.page_id &&
      bpage->residency_generation == identity.residency_generation &&
      buf_page_in_file(bpage) && !bpage->was_stale() && source != nullptr &&
      (buf_page_peek_if_too_old(bpage) || skip_too_old_recheck_for_test)) {
    buf_lru_group_pair_lock(source, staging);

    /* Authoritative re-check: nothing above rules out a concurrent detach
    between reading bpage->lru_group and locking it (e.g. the source group
    itself being merged by compaction, which requires topology-X and so
    cannot race the move below, but could have completed in the gap
    between this thread's S-lock and its block-mutex lock). */
    if (source->pages[bpage->lru_slot] == bpage &&
        bpage->residency_generation == identity.residency_generation) {
      const bool was_old = bpage->old.load(std::memory_order_relaxed);

      source->pages[bpage->lru_slot] = nullptr;
      source->occupied_slots &= ~(uint32_t{1} << bpage->lru_slot);
      source->n_pages--;
      bpage->lru_group = nullptr;

      buf_lru_group_append_page(staging, bpage);
      bpage->old.store(false, std::memory_order_relaxed);

      if (was_old) {
        buf_LRU_old_len_dec(buf_pool);
        buf_pool->stat.n_pages_made_young++;
      }

      if (source->n_pages == 0) {
        result.emptied_source = source;
      }
      result.staged = true;
    }

    buf_lru_group_pair_unlock(source, staging);
  }

  mutex_exit(block_mutex);
  buf_pool->LRU_topology_latch.s_unlock();

  if (result.staged) {
    /* Deterministic reproducer for PS-11141 Requirement 16's skip branch
    (buf_LRU_detach_from_group() encountering a non-LINKED group): no lock
    is held here, so it is safe to simulate the race a concurrent
    buf_page_make_young()/buf_page_make_old() caller would hit against this
    same page while it sits in the still-unpublished staging group.
    buf_LRU_make_block_young() is self-locking as of Step 7 and will try
    its own topology-S fast path first; since bpage's group is STAGING
    (not LINKED), buf_LRU_try_reclassify_S() bails and it falls through to
    the topology-X path this hook means to exercise, same as before. */
    DBUG_EXECUTE_IF("buf_lru_promote_stage_force_conflicting_reclassify",
                    buf_LRU_make_block_young(bpage););
  }

  return result;
}

/** Drain a bounded chunk of deferred make-young identities.

Entries carry values rather than descriptor pointers. Each current residency
is resolved under its page-hash latch and pinned only by this consumer before
ordinary LRU mutation. LRU_drain_mutex preserves single-consumer operation.

PS-11141 Requirements 5/6: the per-page move runs under topology-S plus the
source/staging group mutexes (buf_LRU_stage_promote_page()) instead of
holding topology-X for the whole chunk. A single topology-X phase at the
end publishes the one staging group at the young head, reclaims any source
groups the chunk emptied (via the Step 4 empty-candidate machinery), and
performs one bounded old-boundary adjustment. The staging group is never
retained across activations: a chunk that stages nothing destroys it
before returning. */
bool buf_LRU_drain_promote_queue(buf_pool_t *buf_pool) {
  ut_ad(!buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(!mutex_own(&buf_pool->LRU_drain_mutex));

  if (!buf_pool->LRU_accept_promotions.load(std::memory_order_acquire)) {
    return false;
  }

  auto *const queue = buf_pool->LRU_promote_queue;
  if (queue->empty()) {
    return false;
  }

  mutex_enter(&buf_pool->LRU_drain_mutex);

  if (!buf_pool->LRU_accept_promotions.load(std::memory_order_acquire)) {
    mutex_exit(&buf_pool->LRU_drain_mutex);
    return false;
  }

  std::array<std::optional<buf_lru_promote_t>, BUF_LRU_PROMOTE_DRAIN_CHUNK>
      identities{};
  std::array<buf_page_t *, BUF_LRU_PROMOTE_DRAIN_CHUNK> pinned{};
  uint32_t consumed = 0;

  for (; consumed < BUF_LRU_PROMOTE_DRAIN_CHUNK; ++consumed) {
    const auto identity = queue->try_pop();
    if (!identity.has_value()) {
      break;
    }
    identities[consumed] = identity;
    pinned[consumed] = buf_LRU_pin_promote_identity(buf_pool, *identity);
  }

  /* Allocated outside every lock (buf_lru_group_create() needs none) and
  never taken from the shared reserve (PS-11141 Requirement 5). */
  buf_lru_group_t *staging = buf_lru_group_create();
  staging->state = buf_lru_group_state_t::STAGING;

  std::array<buf_lru_group_t *, BUF_LRU_PROMOTE_DRAIN_CHUNK> emptied_sources{};
  uint32_t n_emptied = 0;

  for (uint32_t i = 0; i < consumed; ++i) {
    buf_page_t *bpage = pinned[i];
    if (bpage == nullptr) {
      continue;
    }
    /* consumed <= BUF_LRU_PROMOTE_DRAIN_CHUNK == BUF_LRU_GROUP_SIZE, and
    this loop stages at most one page per iteration, so staging->n_pages
    can never reach BUF_LRU_GROUP_SIZE before the last iteration runs. */
    ut_a(staging->n_pages < BUF_LRU_GROUP_SIZE);
    const auto staged =
        buf_LRU_stage_promote_page(buf_pool, staging, *identities[i], bpage);
    if (staged.staged && staged.emptied_source != nullptr &&
        n_emptied < BUF_LRU_PROMOTE_DRAIN_CHUNK) {
      emptied_sources[n_emptied++] = staged.emptied_source;
    }
  }

  /* Widens the gap between the S-phase above and the topology-X phase
  below for testing (PS-11141 Requirement 16): with this active, a
  concurrent buf_page_make_old() (the one X-mode path that does not take
  LRU_drain_mutex; see buf_LRU_detach_from_group()) has a real window to
  observe a page sitting in the still-unlinked, STAGING-classified
  `staging` group. */
  DBUG_EXECUTE_IF("buf_lru_promote_stage_before_publish",
                  std::this_thread::sleep_for(std::chrono::microseconds(100)););

  buf_pool->LRU_topology_latch.x_lock();

  for (uint32_t i = 0; i < n_emptied; ++i) {
    buf_lru_group_t *source = emptied_sources[i];
    if (source->n_pages == 0 &&
        source->state == buf_lru_group_state_t::LINKED) {
      buf_LRU_group_became_empty(buf_pool, source);
    }
  }

  bool published = false;
  if (staging->n_pages > 0) {
    /* A batch that moved at least one page is exactly the sparsening
    event LRU_compaction_pending exists to signal; see
    buf_LRU_detach_from_group(). Bumped once per batch here rather than
    once per page, matching Requirement 6's "at most a fixed number of
    ... steps" batching -- this field is a hint for the background
    compactor, not a correctness-critical exact count. */
    buf_pool->LRU_compaction_pending.store(true, std::memory_order_relaxed);
    buf_pool->LRU_compaction_epoch.fetch_add(1, std::memory_order_relaxed);

    /* buf_lru_group_create() deliberately assigns no reuse_generation --
    that counter is protected by topology-X (see buf_lru_group_alloc()),
    which this drain-private staging group is created without holding.
    Assign it now, under the X already held for publication. */
    const uint64_t generation = buf_pool->LRU_group_next_reuse_generation++;
    ut_a(generation != 0);
    staging->reuse_generation = generation;

    staging->state = buf_lru_group_state_t::LINKED;
    UT_LIST_ADD_FIRST(buf_pool->LRU, staging);
    published = true;

    if (buf_pool->LRU_old != nullptr) {
      buf_LRU_old_adjust_len(buf_pool);
    }
  }

  buf_pool->LRU_topology_latch.x_unlock();

  if (!published) {
    buf_lru_group_destroy(staging);
  }

  for (uint32_t i = 0; i < consumed; ++i) {
    if (pinned[i] != nullptr) {
      buf_block_unfix(pinned[i]);
    }
    const bool released = buf_pool->LRU_promote_dedup->release(
        identities[i]->residency_generation);
    ut_ad(released);
    static_cast<void>(released);
  }

  const bool backlog = !queue->reset_wakeup_if_empty();
  mutex_exit(&buf_pool->LRU_drain_mutex);
  return backlog;
}

/** True if compaction must leave this active classification group alone. */
static bool buf_lru_group_is_active(const buf_pool_t *buf_pool,
                                    const buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  return group == buf_pool->LRU_old || group == buf_pool->LRU_fill_group ||
         group == buf_pool->LRU_young_fill_group;
}

/** True if an active unlocked scan currently hazards this group. */
static bool buf_lru_group_has_live_hazard(const buf_pool_t *buf_pool,
                                          const buf_lru_group_t *group) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  return group == buf_pool->lru_hp.get() ||
         (buf_pool->LRU_scan_owner.is_owned() &&
          group == buf_pool->lru_scan_itr.get()) ||
         (buf_pool->LRU_single_scan_active.load(std::memory_order_acquire) !=
              0 &&
          group == buf_pool->single_scan_itr.get());
}

/** Merge pages from right into left, preserving flattened page order, then
reclaim right. Caller holds topology-X; both groups are eligible.
@return true if a merge was performed */
static bool buf_lru_group_try_merge(buf_pool_t *buf_pool, buf_lru_group_t *left,
                                    buf_lru_group_t *right) {
  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(UT_LIST_GET_NEXT(LRU, left) == right);

  if (left->old != right->old ||
      left->n_pages + right->n_pages > BUF_LRU_GROUP_SIZE ||
      left->n_pages == 0 || right->n_pages == 0) {
    return false;
  }

  std::array<buf_page_t *, BUF_LRU_GROUP_SIZE> ordered{};
  uint32_t n = 0;
  for (uint32_t slot = 0; slot < BUF_LRU_GROUP_SIZE; ++slot) {
    if (left->pages[slot] != nullptr) {
      ordered[n++] = left->pages[slot];
    }
  }
  for (uint32_t slot = 0; slot < BUF_LRU_GROUP_SIZE; ++slot) {
    if (right->pages[slot] != nullptr) {
      ordered[n++] = right->pages[slot];
    }
  }
  ut_ad(n == left->n_pages + right->n_pages);

  left->pages.fill(nullptr);
  left->occupied_slots = 0;
  right->pages.fill(nullptr);
  right->occupied_slots = 0;
  right->n_pages = 0;

  for (uint32_t slot = 0; slot < n; ++slot) {
    buf_page_t *bpage = ordered[slot];
    left->pages[slot] = bpage;
    left->occupied_slots |= uint32_t{1} << slot;
    bpage->lru_group = left;
    bpage->lru_slot = slot;
  }
  left->n_pages = n;

  buf_LRU_reclaim_empty_group(buf_pool, right);
  return true;
}

/** Merge eligible adjacent sparse LRU groups under fixed scan and merge
budgets. A persistent reverse hazard cursor makes each activation O(budget)
and completes a tail-to-head sweep across activations.
@param[in,out] buf_pool buffer pool instance */
bool buf_LRU_compact_sparse_groups(buf_pool_t *buf_pool) {
  ut_ad(!buf_pool->LRU_topology_latch.owns_x());
  ut_ad(!mutex_own(&buf_pool->LRU_drain_mutex));

  if (!buf_pool->LRU_accept_promotions.load(std::memory_order_acquire)) {
    return false;
  }

  mutex_enter(&buf_pool->LRU_drain_mutex);

  if (!buf_pool->LRU_accept_promotions.load(std::memory_order_acquire)) {
    mutex_exit(&buf_pool->LRU_drain_mutex);
    return false;
  }

  buf_pool->LRU_topology_latch.x_lock();

  if (!buf_pool->LRU_compaction_pending.load(std::memory_order_relaxed)) {
    buf_pool->LRU_topology_latch.x_unlock();
    const bool maintenance_pending =
        buf_LRU_maintain_group_cache(buf_pool, false);
    mutex_exit(&buf_pool->LRU_drain_mutex);
    return maintenance_pending;
  }

  uint32_t examined = 0;
  uint32_t merges = 0;
  buf_lru_group_t *right = buf_pool->lru_compact_hp.get();
  if (right == nullptr) {
    right = UT_LIST_GET_LAST(buf_pool->LRU);
    buf_pool->LRU_compaction_sweep_epoch =
        buf_pool->LRU_compaction_epoch.load(std::memory_order_relaxed);
    buf_pool->LRU_compaction_retry = false;
  }

  const auto finish_sweep = [buf_pool]() {
    buf_pool->lru_compact_hp.set(nullptr);
    bool pending =
        buf_pool->LRU_compaction_epoch.load(std::memory_order_relaxed) !=
        buf_pool->LRU_compaction_sweep_epoch;
    pending |= buf_pool->LRU_compaction_retry;
    buf_pool->LRU_compaction_pending.store(pending, std::memory_order_relaxed);
  };

  while (right != nullptr && examined < BUF_LRU_COMPACT_SCAN_BUDGET &&
         merges < BUF_LRU_COMPACT_MERGE_BUDGET) {
    buf_lru_group_t *left = UT_LIST_GET_PREV(LRU, right);
    if (left == nullptr) {
      finish_sweep();
      break;
    }

    /* Advance the hazard before right can be reclaimed by a merge. */
    buf_pool->lru_compact_hp.set(left);
    ++examined;

    const bool live_hazard = buf_lru_group_has_live_hazard(buf_pool, left) ||
                             buf_lru_group_has_live_hazard(buf_pool, right);
    buf_pool->LRU_compaction_retry |= live_hazard;

    if (!live_hazard && !buf_lru_group_is_active(buf_pool, left) &&
        !buf_lru_group_is_active(buf_pool, right) &&
        buf_lru_group_try_merge(buf_pool, left, right)) {
      ++merges;
    }

    right = left;
  }

  if (right == nullptr) {
    finish_sweep();
  }

  ut_ad(examined <= BUF_LRU_COMPACT_SCAN_BUDGET);
  ut_ad(merges <= BUF_LRU_COMPACT_MERGE_BUDGET);

  /* A completed sweep that found a live scan hazard must be retried, but an
  immediate coordinator self-wake cannot make that hazard disappear. Keep
  the pending bit for a later periodic/external wake without reporting
  runnable work now. An epoch change or a persistent cursor still represents
  immediately runnable compaction. */
  const bool hazard_only_retry =
      buf_pool->LRU_compaction_pending.load(std::memory_order_relaxed) &&
      buf_pool->lru_compact_hp.get() == nullptr &&
      buf_pool->LRU_compaction_retry &&
      buf_pool->LRU_compaction_epoch.load(std::memory_order_relaxed) ==
          buf_pool->LRU_compaction_sweep_epoch;
  const bool compaction_runnable =
      buf_pool->LRU_compaction_pending.load(std::memory_order_relaxed) &&
      !hazard_only_retry;
  buf_pool->LRU_topology_latch.x_unlock();
  const bool maintenance_pending =
      buf_LRU_maintain_group_cache(buf_pool, false);
  mutex_exit(&buf_pool->LRU_drain_mutex);
  return compaction_runnable || maintenance_pending;
}

/** Stop accepting new deferred promotions.

Published identities are deliberately retained. A producer that passed the
gate before this store may still finish publication, so destructive discard
would require foreground producer rendezvous. Retention is safe because
residency generations never repeat during the pool lifetime; after
invalidation these entries resolve as stale, and at shutdown the queue
outlives all producer threads.
@param[in,out] buf_pool buffer pool instance */
void buf_LRU_close_promote_queue(buf_pool_t *buf_pool) {
  buf_pool->LRU_accept_promotions.store(false, std::memory_order_release);

  /* Rendezvous with a consumer that passed its open check before the store.
  Compaction and drain both take this mutex while accept is still true, so
  close waits them out before invalidation empties the group caches. */
  mutex_enter(&buf_pool->LRU_drain_mutex);
  mutex_exit(&buf_pool->LRU_drain_mutex);
}

/** Re-enable deferred promotion production after invalidation.
@param[in,out] buf_pool buffer pool instance */
void buf_LRU_open_promote_queue(buf_pool_t *buf_pool) {
  buf_pool->LRU_accept_promotions.store(true, std::memory_order_release);
  if (!buf_pool->LRU_promote_queue->empty()) {
    os_event_set(buf_flush_event);
  }
}

bool buf_LRU_free_page(buf_page_t *bpage, bool zip) {
  auto buf_pool = buf_pool_from_bpage(bpage);
  auto block_mutex = buf_page_get_mutex(bpage);
  auto hash_lock = buf_page_hash_lock_get(buf_pool, bpage->id);

  /* PS-11141 Requirement 8: the caller may hold either topology mode.
  buf_LRU_try_evict_tail_identity() holds topology-S for the common,
  uncompressed FILE_PAGE case; every other caller (the keep-zip path via
  zip == false, admin/error paths) still holds topology-X. Whichever mode
  is held on entry is the mode this function unlocks internally on its
  success paths below -- see buf_lru_topology_unlock(). */
  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(mutex_own(block_mutex));

  if (!buf_page_can_relocate(bpage)) {
    /* Do not free buffer fixed and I/O-fixed blocks. */
    return (false);
  }

  /* These assertions can only be checked after the io-fix check
  above, because pages become visible in the page hash table before
  they are linked into the LRU list (they are io-fixed for read
  until after they are linked into the LRU list, so
  buf_page_can_relocate() has returned false for them). */
  ut_ad(bpage->in_LRU_list);
  ut_ad(buf_page_in_file(bpage));

#ifdef UNIV_IBUF_COUNT_DEBUG
  ut_a(ibuf_count_get(bpage->id) == 0);
#endif /* UNIV_IBUF_COUNT_DEBUG */

  buf_page_t *b{};
  auto is_dirty = bpage->is_dirty();

  if (zip || bpage->zip.data == nullptr) {
    /* This would completely free the block. */
    /* Do not completely free dirty blocks. */

    if (is_dirty) {
      return (false);
    }
  } else if (is_dirty && buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
    ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_DIRTY);

    return (false);

  } else if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {
    /* This holds because of the "else" keyword above,
    but we assert it for clarity. */
    ut_ad(!zip && bpage->zip.data != nullptr);
    b = buf_page_alloc_descriptor();
    ut_a(b);
  }

  /* Protect: buf_buddy_free must not be invoked in buf_LRU_block_remove_hashed,
  when passing keep_hash_lock = true (b != nullptr). */
  ut_ad(b == nullptr || (!zip && bpage->zip.data != nullptr));

  ut_ad(buf_page_in_file(bpage));
  ut_ad(bpage->in_LRU_list);
  ut_ad(bpage->in_flush_list == is_dirty);

  DBUG_PRINT("ib_buf", ("free page " UINT32PF ":" UINT32PF, bpage->id.space(),
                        bpage->id.page_no()));

  /* PS-11141 Requirement 8/9: captured before releasing block_mutex below,
  so the re-check after reacquiring it can detect whether a concurrent
  topology-S caller of this same function (another on-demand eviction, or
  the LRU flush scan) won the race for this exact page in the meantime --
  fully freeing it back to the free list, or even recycling the
  descriptor for an unrelated fresh read, before we got block_mutex back.
  Impossible under topology-X, which excludes every other topology user
  for this call's entire duration; a real, observed race once multiple
  concurrent topology-S callers exist. */
  const page_id_t original_id = bpage->id;
  const auto original_generation = bpage->residency_generation;

  mutex_exit(block_mutex);
  DBUG_EXECUTE_IF("buf_lru_free_page_delay_block_mutex_reacquisition",
                  std::this_thread::sleep_for(std::chrono::microseconds(100)););

  rw_lock_x_lock(hash_lock, UT_LOCATION_HERE);
  mutex_enter(block_mutex);

  if (!buf_page_in_file(bpage) || bpage->id != original_id ||
      bpage->residency_generation != original_generation) {
    rw_lock_x_unlock(hash_lock);

    if (b != nullptr) {
      buf_page_free_descriptor(b);
    }

    return (false);
  }

  is_dirty = bpage->is_dirty();

  if (!buf_page_can_relocate(bpage) ||
      ((zip || bpage->zip.data == nullptr) && is_dirty)) {
    rw_lock_x_unlock(hash_lock);

    if (b != nullptr) {
      buf_page_free_descriptor(b);
    }

    return (false);
  }

  if (is_dirty && buf_page_get_state(bpage) != BUF_BLOCK_FILE_PAGE) {
    ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_DIRTY);

    rw_lock_x_unlock(hash_lock);

    if (b != nullptr) {
      buf_page_free_descriptor(b);
    }

    return (false);
  }

  if (b != nullptr) {
    new (b) buf_page_t(*bpage);
  }

  ut_ad(rw_lock_own(hash_lock, RW_LOCK_X));
  ut_ad(buf_page_can_relocate(bpage));

  /* When the compressed page is to be kept (b != nullptr), ask
  buf_LRU_block_remove_hashed() to keep the hash cell X-latch, so that it
  is held continuously from before the HASH_DELETE until after the
  compressed-only descriptor is re-inserted below: the page id is never
  observably absent from the page hash. Otherwise a concurrent
  buf_page_init_for_read() (which inserts into the page hash without
  holding the topology latch) could insert a second descriptor for this
  page id in the meantime.

  Protect: buf_buddy_free must not be invoked in buf_LRU_block_remove_hashed,
  when passing keep_hash_lock = true (b != nullptr). */
  ut_ad(b == nullptr || (!zip && bpage->zip.data != nullptr));

  if (!buf_LRU_block_remove_hashed(bpage, zip, false, b != nullptr)) {
    buf_lru_topology_unlock(buf_pool);

    if (b != nullptr) {
      buf_page_free_descriptor(b);
    }
    return true;
  }
  ut_ad(!mutex_own(block_mutex));

  /* buf_LRU_block_remove_hashed() releases the hash_lock, unless it was
  asked to keep it for the re-insert of the compressed-only descriptor. */
  ut_ad(b != nullptr ? rw_lock_own(hash_lock, RW_LOCK_X)
                     : (!rw_lock_own(hash_lock, RW_LOCK_X) &&
                        !rw_lock_own(hash_lock, RW_LOCK_S)));

  /* We have just freed a BUF_BLOCK_FILE_PAGE. If b != nullptr
  then it was a compressed page with an uncompressed frame and
  we are interested in freeing only the uncompressed frame.
  Therefore we have to reinsert the compressed page descriptor
  into the LRU and page_hash (and possibly flush_list).
  if b == nullptr then it was a regular page that has been freed */

  /* Widen the window between the page hash delete and the re-insert of
  the compressed-only descriptor. The hash cell X-latch is held here
  (keep_hash_lock), so a concurrent buf_page_init_for_read() of this page
  must block on the cell latch instead of inserting a second descriptor
  for the same page id into the gap. */
  DBUG_EXECUTE_IF(
      "buf_lru_free_page_delay_zip_reinsert", if (b != nullptr) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      });

  if (b != nullptr) {
    /* The hash cell X-latch has been held continuously since before the
    HASH_DELETE in buf_LRU_block_remove_hashed() (see keep_hash_lock),
    which is what makes the assertion below provable: no other thread can
    have inserted a descriptor for this page id in the meantime. */
    ut_ad(rw_lock_own(hash_lock, RW_LOCK_X));

    mutex_enter(block_mutex);

    ut_a(!buf_page_hash_get_low(buf_pool, b->id));

    b->state = b->is_dirty() ? BUF_BLOCK_ZIP_DIRTY : BUF_BLOCK_ZIP_PAGE;

    ut_ad(b->size.is_compressed());

    UNIV_MEM_DESC(b->zip.data, b->size.physical());

    /* The fields in_page_hash and in_LRU_list of
    the to-be-freed block descriptor should have
    been cleared in
    buf_LRU_block_remove_hashed(), which
    invokes buf_LRU_remove_block(). */
    ut_ad(!bpage->in_page_hash);
    ut_ad(!bpage->in_LRU_list);

    /* bpage->state was BUF_BLOCK_FILE_PAGE because
    b != NULL. The type cast below is thus valid. */
    ut_ad(!((buf_block_t *)bpage)->in_unzip_LRU_list);

    /* The fields of bpage were copied to b before
    buf_LRU_block_remove_hashed() was invoked. */
    ut_ad(!b->in_zip_hash);
    ut_ad(b->in_page_hash);
    ut_ad(b->in_LRU_list);

    HASH_INSERT(buf_page_t, hash, buf_pool->page_hash, b->id.hash(), b);

    /* Reinsert b as a fresh page (PS-11141 grouped LRU list): unlike the
    pre-grouping code, there is no attempt to preserve bpage's exact former
    position -- buf_LRU_remove_block() (via buf_LRU_block_remove_hashed()
    above) has already fully removed bpage from its group, so b is a new
    insertion like any other, old-ness-wise equivalent to bpage's. This is
    consistent with the coarser positional precision the grouped design
    already accepts elsewhere (fill groups, best-effort partial eviction). */
    ut_d(b->in_LRU_list = false);
    buf_LRU_add_block_low(b, buf_page_is_old(b));

    mutex_enter(&buf_pool->zip_mutex);
    rw_lock_x_unlock(hash_lock);
    if (b->state == BUF_BLOCK_ZIP_PAGE) {
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
      buf_LRU_insert_zip_clean(b);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
    } else {
      /* Relocate on buf_pool->flush_list. */
      buf_flush_relocate_on_flush_list(bpage, b);
    }

    bpage->zip.data = nullptr;

    page_zip_set_size(&bpage->zip, 0);

    bpage->size.copy_from(
        page_size_t(bpage->size.logical(), bpage->size.logical(), false));

    /* Prevent buf_page_get_gen() from
    decompressing the block while we release block_mutex. */

    buf_page_set_sticky(b);

    mutex_exit(&buf_pool->zip_mutex);

    mutex_exit(block_mutex);
  }

  buf_lru_topology_unlock(buf_pool);

  /* Remove possible adaptive hash index on the page.
  The page was declared uninitialized by
  buf_LRU_block_remove_hashed().  We need to flag
  the contents of the page valid (which it still is) in
  order to avoid bogus Valgrind warnings.*/
  UNIV_MEM_VALID(((buf_block_t *)bpage)->frame, UNIV_PAGE_SIZE);
  btr_search_drop_page_hash_index((buf_block_t *)bpage, true);
  UNIV_MEM_INVALID(((buf_block_t *)bpage)->frame, UNIV_PAGE_SIZE);

  if (b != nullptr) {
    /* Compute and stamp the compressed page
    checksum while not holding any mutex.  The
    block is already half-freed
    (BUF_BLOCK_REMOVE_HASH) and removed from
    buf_pool->page_hash, thus inaccessible by any
    other thread. */

    ut_ad(b->size.is_compressed());

    BlockReporter reporter = BlockReporter(false, b->zip.data, b->size, false);

    const uint32_t checksum = reporter.calc_zip_checksum(
        static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm));

    mach_write_to_4(b->zip.data + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
  }

  if (b != nullptr) {
    mutex_enter(&buf_pool->zip_mutex);

    buf_page_unset_sticky(b);

    mutex_exit(&buf_pool->zip_mutex);
  }

  buf_LRU_block_free_hashed_page((buf_block_t *)bpage);

  return (true);
}

/** Puts a block back to the free list.
@param[in]  block  block must not contain a file page */
void buf_LRU_block_free_non_file_page(buf_block_t *block) {
  void *data;
  buf_pool_t *buf_pool = buf_pool_from_block(block);

  switch (buf_block_get_state(block)) {
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_READY_FOR_USE:
      break;
    default:
      ut_error;
  }

  block->ahi.assert_empty();
  ut_ad(!block->page.in_free_list);
  ut_ad(!block->page.in_flush_list);
  ut_ad(!block->page.in_LRU_list);

#ifdef UNIV_DEBUG
  /* Wipe contents of page to reveal possible stale pointers to it */
  memset(block->frame, '\0', UNIV_PAGE_SIZE);
#else
  /* Wipe page_no and space_id */
  memset(block->frame + FIL_PAGE_OFFSET, 0xfe, 4);
  memset(block->frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xfe, 4);
#endif /* UNIV_DEBUG */
  UNIV_MEM_ASSERT_AND_FREE(block->frame, UNIV_PAGE_SIZE);
  data = block->page.zip.data;

  if (data != nullptr) {
    block->page.zip.data = nullptr;

    ut_ad(block->page.size.is_compressed());

    buf_buddy_free(buf_pool, data, block->page.size.physical());

    page_zip_set_size(&block->page.zip, 0);

    block->page.size.copy_from(page_size_t(block->page.size.logical(),
                                           block->page.size.logical(), false));
  }

#ifndef UNIV_HOTBACKUP
  buf_page_prepare_for_free(&block->page);
  ut_ad(block->page.get_space() == nullptr);
#endif /* !UNIV_HOTBACKUP */

  if (buf_get_withdraw_depth(buf_pool) &&
      buf_block_will_withdrawn(buf_pool, block)) {
    /* This should be withdrawn */
    buf_block_set_state(block, BUF_BLOCK_NOT_USED);
    mutex_enter(&buf_pool->free_list_mutex);
    UT_LIST_ADD_LAST(buf_pool->withdraw, &block->page);
    ut_d(block->in_withdraw_list = true);
    mutex_exit(&buf_pool->free_list_mutex);
  } else {
    buf_block_set_state(block, BUF_BLOCK_NOT_USED);
    mutex_enter(&buf_pool->free_list_mutex);
    UT_LIST_ADD_FIRST(buf_pool->free, &block->page);
    ut_d(block->page.in_free_list = true);
    ut_ad(!block->page.someone_has_io_responsibility());
    mutex_exit(&buf_pool->free_list_mutex);
  }
}

/** Takes a block out of the LRU list and page hash table.
If the block is compressed-only (BUF_BLOCK_ZIP_PAGE),
the object will be freed.

The caller must hold buf_pool->LRU_topology_latch, the buf_page_get_mutex()
mutex and the appropriate hash_lock. This function will release the
buf_page_get_mutex() and the hash_lock (the latter is kept if keep_hash_lock
is passed).

If a compressed page is freed other compressed pages may be relocated.

@param[in]      bpage           block, must contain a file page and
                                be in a state where it can be freed; there
                                may or may not be a hash index to the page
@param[in]      zip             true if should remove also the
                                compressed page of an uncompressed page
@param[in]      ignore_content  true if should ignore page content, since it
                                could be not initialized
@param[in]      keep_hash_lock  true if the hash cell X-latch should be kept
                                by this function instead of being released;
                                only allowed when the caller will re-insert
                                a compressed-only descriptor for this page
                                id into the page hash (the keep-zip path of
                                buf_LRU_free_page()), so that the page id is
                                never observably absent from the page hash
@retval true if BUF_BLOCK_FILE_PAGE was removed from page_hash. The
caller needs to free the page to the free list
@retval false if BUF_BLOCK_ZIP_PAGE was removed from page_hash. In
this case the block is already returned to the buddy allocator. */
static bool buf_LRU_block_remove_hashed(buf_page_t *bpage, bool zip,
                                        bool ignore_content,
                                        bool keep_hash_lock) {
  const buf_page_t *hashed_bpage;
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  rw_lock_t *hash_lock;

  ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
  ut_ad(mutex_own(buf_page_get_mutex(bpage)));

  /* keep_hash_lock is only supported for the keep-zip path of
  buf_LRU_free_page(): an uncompressed frame being freed while its
  compressed page is kept and will be re-inserted into the page hash.
  In particular the buddy allocator must not be invoked while the hash
  cell X-latch is kept (buf_buddy_free() may take page hash latches via
  buf_buddy_relocate()), which is guaranteed by zip == false. Compressed-page
  territory is deliberately deferred throughout this effort (Step 5), so
  this path stays topology-X only -- buf_LRU_try_evict_tail_identity()
  never attempts topology-S for a page with zip.data set. */
  ut_ad(!keep_hash_lock ||
        (!zip && buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE &&
         bpage->zip.data != nullptr));
  ut_ad(!keep_hash_lock || buf_pool->LRU_topology_latch.owns_x());

  hash_lock = buf_page_hash_lock_get(buf_pool, bpage->id);

  ut_ad(rw_lock_own(hash_lock, RW_LOCK_X));

  ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
  ut_a(bpage->buf_fix_count == 0);

  /* buf_fix_count == 0 above already rules out the staging-skip case
  (PS-11141 Requirement 16): every page in a drain's private staging
  group stays fixed until publication, so this can never be false here.
  buf_LRU_remove_block() itself is dual-mode as of PS-11141 Requirement 8
  and dispatches internally on whichever topology mode this caller holds. */
  ut_a(buf_LRU_remove_block(bpage));

  buf_pool->freed_page_clock += 1;

  switch (buf_page_get_state(bpage)) {
    case BUF_BLOCK_FILE_PAGE: {
      UNIV_MEM_ASSERT_W(bpage, sizeof(buf_block_t));
      UNIV_MEM_ASSERT_W(((buf_block_t *)bpage)->frame, UNIV_PAGE_SIZE);

      buf_block_modify_clock_inc((buf_block_t *)bpage);

      if (bpage->zip.data != nullptr) {
        const page_t *page = ((buf_block_t *)bpage)->frame;

        ut_a(!zip || !bpage->is_dirty());
        ut_ad(bpage->size.is_compressed());

        switch (fil_page_get_type(page)) {
          case FIL_PAGE_TYPE_ALLOCATED:
          case FIL_PAGE_INODE:
          case FIL_PAGE_IBUF_BITMAP:
          case FIL_PAGE_TYPE_FSP_HDR:
          case FIL_PAGE_TYPE_XDES:
          case FIL_PAGE_TYPE_ZLOB_FIRST:
          case FIL_PAGE_TYPE_ZLOB_DATA:
          case FIL_PAGE_TYPE_ZLOB_INDEX:
          case FIL_PAGE_TYPE_ZLOB_FRAG:
          case FIL_PAGE_TYPE_ZLOB_FRAG_ENTRY:
            /* These are essentially uncompressed pages. */
            if (!zip) {
              /* InnoDB writes the data to the
              uncompressed page frame.  Copy it
              to the compressed page, which will
              be preserved. */
              memcpy(bpage->zip.data, page, bpage->size.physical());
            }
            break;
          case FIL_PAGE_TYPE_ZBLOB:
          case FIL_PAGE_TYPE_ZBLOB2:
          case FIL_PAGE_SDI_ZBLOB:
            break;
          case FIL_PAGE_INDEX:
          case FIL_PAGE_SDI:
          case FIL_PAGE_RTREE:
#ifdef UNIV_ZIP_DEBUG
            ut_a(page_zip_validate(&bpage->zip, page,
                                   ((buf_block_t *)bpage)->index));
#endif /* UNIV_ZIP_DEBUG */
            break;
          default:
            ib::error(ER_IB_MSG_135) << "The compressed page to be"
                                        " evicted seems corrupt:";
            ut_print_buf(stderr, page, bpage->size.logical());

            ib::error(ER_IB_MSG_136) << "Possibly older version of"
                                        " the page:";

            ut_print_buf(stderr, bpage->zip.data, bpage->size.physical());
            putc('\n', stderr);
            ut_error;
        }

        break;
      }

      if (!ignore_content) {
        /* Account the eviction of index leaf pages from
        the buffer pool(s). */

        const byte *frame = bpage->zip.data != nullptr
                                ? bpage->zip.data
                                : reinterpret_cast<buf_block_t *>(bpage)->frame;

        const ulint type = fil_page_get_type(frame);

        if ((type == FIL_PAGE_INDEX || type == FIL_PAGE_RTREE) &&
            page_is_leaf(frame)) {
          uint32_t space_id = bpage->id.space();

          space_index_t idx_id = btr_page_get_index_id(frame);

          buf_stat_per_index->dec(index_id_t(space_id, idx_id));
        }
      }
    }
      [[fallthrough]];
    case BUF_BLOCK_ZIP_PAGE:
      ut_a(!bpage->is_dirty());
      if (bpage->size.is_compressed()) {
        UNIV_MEM_ASSERT_W(bpage->zip.data, bpage->size.physical());
      }
      break;
    case BUF_BLOCK_POOL_WATCH:
    case BUF_BLOCK_ZIP_DIRTY:
    case BUF_BLOCK_NOT_USED:
    case BUF_BLOCK_READY_FOR_USE:
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_REMOVE_HASH:
      ut_error;
      break;
  }

  hashed_bpage = buf_page_hash_get_low(buf_pool, bpage->id);

  if (bpage != hashed_bpage) {
    ib::error(ER_IB_MSG_137)
        << "Page " << bpage->id << " not found in the hash table";

    if (hashed_bpage) {
      ib::error(ER_IB_MSG_138)
          << "In hash table we find block " << hashed_bpage << " of "
          << hashed_bpage->id << " which is not " << bpage;
    }

    ut_d(mutex_exit(buf_page_get_mutex(bpage)));
    ut_d(rw_lock_x_unlock(hash_lock));
    ut_d(buf_pool->LRU_topology_latch.x_unlock());
    ut_d(buf_print());
    ut_d(buf_LRU_print());
    ut_d(buf_validate());
    ut_d(buf_LRU_validate());
    ut_d(ut_error);
  }

  ut_ad(!bpage->in_zip_hash);
  ut_ad(bpage->in_page_hash);
  ut_d(bpage->in_page_hash = false);

  HASH_DELETE(buf_page_t, hash, buf_pool->page_hash, bpage->id.hash(), bpage);

  switch (buf_page_get_state(bpage)) {
    case BUF_BLOCK_ZIP_PAGE:
      ut_ad(!bpage->in_free_list);
      ut_ad(!bpage->in_flush_list);
      ut_ad(!bpage->in_LRU_list);
      ut_a(bpage->zip.data);
      ut_a(bpage->size.is_compressed());

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
      UT_LIST_REMOVE(buf_pool->zip_clean, bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

      mutex_exit(&buf_pool->zip_mutex);
      rw_lock_x_unlock(hash_lock);

      buf_buddy_free(buf_pool, bpage->zip.data, bpage->size.physical());

      buf_page_free_descriptor(bpage);
      return (false);

    case BUF_BLOCK_FILE_PAGE:
      memset(((buf_block_t *)bpage)->frame + FIL_PAGE_OFFSET, 0xff, 4);
      memset(((buf_block_t *)bpage)->frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
             0xff, 4);
      UNIV_MEM_INVALID(((buf_block_t *)bpage)->frame, UNIV_PAGE_SIZE);
      buf_page_set_state(bpage, BUF_BLOCK_REMOVE_HASH);

      /* Question: If we release bpage and hash mutex here
      then what protects us against:
      1) Some other thread buffer fixing this page
      2) Some other thread trying to read this page and
      not finding it in buffer pool attempting to read it
      from the disk.
      Answer:
      1) Cannot happen because the page is no longer in the
      page_hash. Only possibility is when while invalidating
      a tablespace we buffer fix the prev_page in LRU to
      avoid relocation during the scan -- that scan
      (buf_LRU_drop_page_hash_for_tablespace(),
      buf_LRU_remove_all_pages()) always holds topology-X, and
      topology-X and topology-S are mutually exclusive, so holding
      either mode here is equally sufficient to exclude it (PS-11141
      Requirement 8: this path can now also be reached under
      topology-S plus the source group's mutex, via
      buf_LRU_remove_block()'s topology-S path).

      2) When a compressed-only descriptor will be re-inserted
      for this page id (the keep-zip path of buf_LRU_free_page(),
      keep_hash_lock == true), the hash cell X-latch is kept from
      before the HASH_DELETE above until after the re-insert in the
      caller, so the page id is never observably absent from the
      page hash: a concurrent buf_page_init_for_read() blocks on
      the hash cell latch and then finds the compressed-only
      descriptor. (It cannot be the topology latch which protects
      this transition: buf_page_init_for_read() inserts into the
      page hash without holding the topology latch.) The keep-zip
      path itself remains topology-X only (see the assertion at this
      function's entry), so keep_hash_lock == true is never observed
      here under topology-S.
      When nothing will be re-inserted (keep_hash_lock == false),
      the page is leaving the buffer pool for good and a concurrent
      thread reading it from the disk afresh is the normal cache
      miss path. */
      ut_ad(buf_pool->LRU_topology_latch.owns_s_or_x());
      if (!keep_hash_lock) {
        rw_lock_x_unlock(hash_lock);
      }
      mutex_exit(&((buf_block_t *)bpage)->mutex);

      if (zip && bpage->zip.data) {
        /* Free the compressed page. */
        void *data = bpage->zip.data;
        bpage->zip.data = nullptr;

        ut_ad(!bpage->in_free_list);
        ut_ad(!bpage->in_flush_list);
        ut_ad(!bpage->in_LRU_list);

        buf_buddy_free(buf_pool, data, bpage->size.physical());

        page_zip_set_size(&bpage->zip, 0);

        bpage->size.copy_from(
            page_size_t(bpage->size.logical(), bpage->size.logical(), false));
      }

      return (true);

    case BUF_BLOCK_POOL_WATCH:
    case BUF_BLOCK_ZIP_DIRTY:
    case BUF_BLOCK_NOT_USED:
    case BUF_BLOCK_READY_FOR_USE:
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_REMOVE_HASH:
      break;
  }

  ut_error;
}

static void buf_LRU_block_free_hashed_page(buf_block_t *block) noexcept {
  buf_block_set_state(block, BUF_BLOCK_MEMORY);

  buf_LRU_block_free_non_file_page(block);
}

void buf_LRU_free_one_page(buf_page_t *bpage, bool ignore_content) {
#ifdef UNIV_DEBUG
  buf_pool_t *buf_pool = buf_pool_from_bpage(bpage);
  BPageMutex *block_mutex = buf_page_get_mutex(bpage);
  rw_lock_t *hash_lock = buf_page_hash_lock_get(buf_pool, bpage->id);

  ut_ad(buf_pool->LRU_topology_latch.owns_x());
  ut_ad(mutex_own(block_mutex));
  ut_ad(rw_lock_own(hash_lock, RW_LOCK_X));
#endif /* UNIV_DEBUG */

  if (buf_LRU_block_remove_hashed(bpage, true, ignore_content)) {
    buf_LRU_block_free_hashed_page((buf_block_t *)bpage);
  }

  /* buf_LRU_block_remove_hashed() releases hash_lock and block_mutex */
  ut_ad(!rw_lock_own(hash_lock, RW_LOCK_X) &&
        !rw_lock_own(hash_lock, RW_LOCK_S));

  ut_ad(!mutex_own(block_mutex));
}

/** Updates buf_pool->LRU_old_ratio for one buffer pool instance.
@param[in]      buf_pool        buffer pool instance
@param[in]      old_pct         Reserve this percentage of
                                the buffer pool for "old" blocks
@param[in]      adjust          true=adjust the LRU list;
                                false=just assign buf_pool->LRU_old_ratio
                                during the initialization of InnoDB
@return updated old_pct */
static uint buf_LRU_old_ratio_update_instance(buf_pool_t *buf_pool,
                                              uint old_pct, bool adjust) {
  uint ratio;

  ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100;
  if (ratio < BUF_LRU_OLD_RATIO_MIN) {
    ratio = BUF_LRU_OLD_RATIO_MIN;
  } else if (ratio > BUF_LRU_OLD_RATIO_MAX) {
    ratio = BUF_LRU_OLD_RATIO_MAX;
  }

  if (adjust) {
    buf_pool->LRU_topology_latch.x_lock();

    if (ratio != buf_pool->LRU_old_ratio) {
      buf_pool->LRU_old_ratio = ratio;

      if (buf_pool->LRU_n_pages >= BUF_LRU_OLD_MIN_LEN) {
        buf_LRU_old_adjust_len(buf_pool);
      }
    }

    buf_pool->LRU_topology_latch.x_unlock();
  } else {
    buf_pool->LRU_old_ratio = ratio;
  }
  /* the reverse of
  ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100 */
  return ((uint)(ratio * 100 / (double)BUF_LRU_OLD_RATIO_DIV + 0.5));
}

/** Updates buf_pool->LRU_old_ratio.
 @return updated old_pct */
uint buf_LRU_old_ratio_update(
    uint old_pct, /*!< in: Reserve this percentage of
                  the buffer pool for "old" blocks. */
    bool adjust)  /*!< in: true=adjust the LRU list;
                   false=just assign buf_pool->LRU_old_ratio
                   during the initialization of InnoDB */
{
  uint new_ratio = 0;

  for (ulint i = 0; i < srv_buf_pool_instances; i++) {
    buf_pool_t *buf_pool;

    buf_pool = buf_pool_from_array(i);

    new_ratio = buf_LRU_old_ratio_update_instance(buf_pool, old_pct, adjust);
  }

  return (new_ratio);
}

/** Update the historical stats that we are collecting for LRU eviction
 policy at the end of each interval. */
void buf_LRU_stat_update(void) {
  buf_LRU_stat_t *item;
  buf_pool_t *buf_pool;
  bool evict_started = false;
  buf_LRU_stat_t cur_stat;

  /* If we haven't started eviction yet then don't update stats. */
  os_rmb;
  for (ulint i = 0; i < srv_buf_pool_instances; i++) {
    buf_pool = buf_pool_from_array(i);

    if (buf_pool->freed_page_clock != 0) {
      evict_started = true;
      break;
    }
  }

  if (!evict_started) {
    goto func_exit;
  }

  /* Update the index. */
  item = &buf_LRU_stat_arr[buf_LRU_stat_arr_ind];
  buf_LRU_stat_arr_ind++;
  buf_LRU_stat_arr_ind %= BUF_LRU_STAT_N_INTERVAL;

  /* Add the current value and subtract the obsolete entry.
  Since buf_LRU_stat_cur is not protected by any mutex,
  it can be changing between adding to buf_LRU_stat_sum
  and copying to item. Assign it to local variables to make
  sure the same value assign to the buf_LRU_stat_sum
  and item */
  cur_stat = buf_LRU_stat_cur;

  buf_LRU_stat_sum.io += cur_stat.io - item->io;
  buf_LRU_stat_sum.unzip += cur_stat.unzip - item->unzip;

  /* Put current entry in the array. */
  memcpy(item, &cur_stat, sizeof *item);

func_exit:
  /* Clear the current entry. */
  memset(&buf_LRU_stat_cur, 0, sizeof buf_LRU_stat_cur);
  os_wmb;
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Validates the LRU list for one buffer pool instance.
@param[in]      buf_pool        buffer pool instance */
void buf_LRU_validate_instance(buf_pool_t *buf_pool) {
  /* Exclude background promotion and compaction for the complete validator,
  including checks performed across separate topology-X sections. */
  mutex_enter(&buf_pool->LRU_drain_mutex);

  buf_pool->LRU_topology_latch.x_lock();

  if (buf_pool->LRU_n_pages >= BUF_LRU_OLD_MIN_LEN) {
    ut_a(buf_pool->LRU_old);

    /* PS-11141 Requirement 10/Step 7: the topology-S insertion and
    make-young/make-old fast paths deliberately do not call
    buf_LRU_old_adjust_len() on every page (see buf_LRU_add_fresh_page()'s
    comment) -- convergence instead rides the natural fill-group-rollover
    cadence. A quiet period between rollovers could otherwise leave
    LRU_old_len outside tolerance of the ratio target below through no
    fault of the exact-count bookkeeping, which is unconditionally
    maintained regardless of that deferral. Force one bounded, idempotent
    adjustment here -- this function already holds topology-X for its
    entire body -- so this assertion checks the design's real invariant
    (bounded convergence is always reachable) instead of an artifact of
    how recently a fill group happened to roll over. */
    buf_LRU_old_adjust_len(buf_pool);

    const size_t new_len = calculate_desired_LRU_old_size(buf_pool);

    ut_a(buf_pool->LRU_old_len >= new_len - BUF_LRU_OLD_TOLERANCE);
    ut_a(buf_pool->LRU_old_len <= new_len + BUF_LRU_OLD_TOLERANCE);
  }

  CheckInLRUList::validate(buf_pool);

  /* PS-11141 grouped LRU list: old groups must form a contiguous suffix
  (no young group after an old one), LRU_old must point at the first old
  group, every page's "old" flag must match its group's, and the
  page/old-page counts must reconcile with LRU_n_pages/LRU_old_len. */
  ulint old_len = 0;
  ulint page_count = 0;
  bool seen_old = false;
  size_t queued_len = 0;

  for (auto *group : buf_pool->LRU) {
    ut_a(group->state == buf_lru_group_state_t::LINKED);
    ut_a(group->reuse_generation != 0);
    if (group->n_pages == 0) {
      /* A group can be empty-but-still-linked while it awaits a deferred
      reclaim (PS-11141 Requirement 8/9): either queued on
      LRU_empty_candidates (n_maintenance_refs > 0) or only found by the
      persistent fallback scan (n_maintenance_refs == 0 but
      empty_candidate_pending). Either way it must be flagged pending;
      nothing empties a group without also publishing it as a candidate. */
      ut_a(group->empty_candidate_pending.load(std::memory_order_relaxed));
      if (group->n_maintenance_refs > 0) {
        ++queued_len;
      }
    } else {
      ut_a(group->n_maintenance_refs == 0);
      ut_a(!group->empty_candidate_pending.load(std::memory_order_relaxed));
    }
    if (group->old) {
      if (!seen_old) {
        ut_a(buf_pool->LRU_old == group);
        seen_old = true;
      }
      old_len += group->n_pages;
    } else {
      ut_a(!seen_old);
    }

    uint32_t observed_slots = 0;
    for (uint32_t slot = 0; slot < BUF_LRU_GROUP_SIZE; ++slot) {
      auto *bpage = group->pages[slot];
      if (bpage == nullptr) {
        continue;
      }
      observed_slots |= uint32_t{1} << slot;
      ++page_count;

      switch (buf_page_get_state(bpage)) {
        case BUF_BLOCK_POOL_WATCH:
        case BUF_BLOCK_NOT_USED:
        case BUF_BLOCK_READY_FOR_USE:
        case BUF_BLOCK_MEMORY:
        case BUF_BLOCK_REMOVE_HASH:
          ut_error;
          break;
        case BUF_BLOCK_FILE_PAGE:
          ut_ad(((buf_block_t *)bpage)->in_unzip_LRU_list ==
                buf_page_belongs_to_unzip_LRU(bpage));
        case BUF_BLOCK_ZIP_PAGE:
        case BUF_BLOCK_ZIP_DIRTY:
          break;
      }

      ut_a(buf_page_is_old(bpage) == group->old);
      ut_a(bpage->lru_group == group);
      ut_a(bpage->lru_slot == slot);
    }
    ut_a(group->occupied_slots == observed_slots);
    ut_a(group->n_pages ==
         static_cast<uint32_t>(std::popcount(observed_slots)));
  }

  ut_a(page_count == buf_pool->LRU_n_pages);
  ut_a(buf_pool->LRU_old_len == old_len);
  ut_a(buf_pool->LRU_group_cache_len <= BUF_LRU_GROUP_RESERVE_MAX);

  size_t reserve_len = 0;
  for (buf_lru_group_t *group = buf_pool->LRU_group_cache; group != nullptr;
       group = group->cache_next) {
    ut_a(group->state == buf_lru_group_state_t::RESERVE);
    ut_a(group->n_pages == 0);
    ut_a(group->occupied_slots == 0);
    ut_a(group->n_maintenance_refs == 0);
    ut_a(!group->empty_candidate_pending.load(std::memory_order_relaxed));
    ++reserve_len;
  }
  ut_a(reserve_len == buf_pool->LRU_group_cache_len);

  size_t retired_len = 0;
  for (buf_lru_group_t *group = buf_pool->LRU_group_retired; group != nullptr;
       group = group->cache_next) {
    ut_a(group->state == buf_lru_group_state_t::RETIRED);
    ut_a(group->n_pages == 0);
    ut_a(group->occupied_slots == 0);
    ut_a(group->n_maintenance_refs == 0);
    ut_a(!group->empty_candidate_pending.load(std::memory_order_relaxed));
    ++retired_len;
  }
  ut_a(retired_len == buf_pool->LRU_group_retired_len);

  /* PS-11141 Requirement 8: LRU_empty_candidates is now a
  Bounded_mpsc_queue, which does not support peeking its FIFO contents
  without popping them (that would perturb draining, still exclusively
  owned by the topology-X consumer even here). Cross-check the aggregate
  count instead: every LINKED empty group with a live maintenance
  reference is queued exactly once (buf_LRU_enqueue_empty_candidate() takes
  the one reference this queue implies, and nothing else does), so the two
  counts must agree. */
  ut_a(buf_pool->LRU_empty_candidates->size() <=
       BUF_LRU_EMPTY_CANDIDATE_QUEUE_CAP);
  ut_a(queued_len == buf_pool->LRU_empty_candidates->size());

  buf_pool->LRU_topology_latch.x_unlock();

  mutex_enter(&buf_pool->free_list_mutex);

  CheckInFreeList::validate(buf_pool);

  for (auto bpage : buf_pool->free) {
    ut_a(buf_page_get_state(bpage) == BUF_BLOCK_NOT_USED);
  }

  mutex_exit(&buf_pool->free_list_mutex);

  buf_pool->LRU_topology_latch.x_lock();

  CheckUnzipLRUAndLRUList::validate(buf_pool);

  for (auto block : buf_pool->unzip_LRU) {
    ut_ad(block->in_unzip_LRU_list);
    ut_ad(block->page.in_LRU_list);
    ut_a(buf_page_belongs_to_unzip_LRU(&block->page));
  }

  buf_pool->LRU_topology_latch.x_unlock();

  mutex_exit(&buf_pool->LRU_drain_mutex);
}

/** Validates the LRU list. */
void buf_LRU_validate(void) {
  for (size_t i = 0; i < srv_buf_pool_instances; i++) {
    buf_pool_t *buf_pool = buf_pool_from_array(i);
    buf_LRU_validate_instance(buf_pool);
  }
}

Space_References buf_LRU_count_space_references() {
  Space_References result;
  for (size_t i = 0; i < srv_buf_pool_instances; i++) {
    buf_pool_t *buf_pool = buf_pool_from_array(i);

    /* Debug-only: exclude background promotion and compaction for the scan. */
    mutex_enter(&buf_pool->LRU_drain_mutex);
    buf_pool->LRU_topology_latch.x_lock();

    /* We have the LRU mutex, it is safe to assume the space ID will not be
    changed, as it would require removal from the LRU first. */
    buf_LRU_for_each_page(
        buf_pool, [&](buf_page_t *bpage) { result[bpage->get_space()]++; });

    for (size_t j = 0; j < BUF_POOL_WATCH_SIZE; j++) {
      const auto &bpage = &buf_pool->watch[j];

      switch (bpage->state) {
        case BUF_BLOCK_ZIP_PAGE:
          result[bpage->get_space()]++;
          break;
        default:
          break;
      }
    }

    buf_pool->LRU_topology_latch.x_unlock();
    mutex_exit(&buf_pool->LRU_drain_mutex);
  }

  return result;
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Prints the LRU list for one buffer pool instance.
@param[in]      buf_pool        buffer pool instance */
static void buf_LRU_print_instance(buf_pool_t *buf_pool) {
  /* PS-11141 grouped LRU list: see the matching comment in
  buf_LRU_count_space_references(). */
  mutex_enter(&buf_pool->LRU_drain_mutex);
  buf_pool->LRU_topology_latch.x_lock();

  buf_LRU_for_each_page(buf_pool, [](buf_page_t *bpage) {
    mutex_enter(buf_page_get_mutex(bpage));

    fprintf(stderr, "BLOCK space " UINT32PF " page " UINT32PF " ",
            bpage->id.space(), bpage->id.page_no());

    if (buf_page_is_old(bpage)) {
      fputs("old ", stderr);
    }

    if (bpage->buf_fix_count) {
      fprintf(stderr, "buffix count %lu ", (ulong)bpage->buf_fix_count);
    }

    if (buf_page_get_io_fix(bpage)) {
      fprintf(stderr, "io_fix %lu ", (ulong)buf_page_get_io_fix(bpage));
    }

    if (bpage->is_dirty()) {
      fputs("modif. ", stderr);
    }

    switch (buf_page_get_state(bpage)) {
      const byte *frame;
      case BUF_BLOCK_FILE_PAGE:
        frame = buf_block_get_frame((buf_block_t *)bpage);
        fprintf(stderr,
                "\ntype %lu"
                " index id " IB_ID_FMT "\n",
                (ulong)fil_page_get_type(frame), btr_page_get_index_id(frame));
        break;
      case BUF_BLOCK_ZIP_PAGE:
        frame = bpage->zip.data;
        fprintf(stderr,
                "\ntype %lu size %lu"
                " index id " IB_ID_FMT "\n",
                (ulong)fil_page_get_type(frame), (ulong)bpage->size.physical(),
                btr_page_get_index_id(frame));
        break;

      default:
        fprintf(stderr, "\n!state %lu!\n", (ulong)buf_page_get_state(bpage));
        break;
    }

    mutex_exit(buf_page_get_mutex(bpage));
  });

  buf_pool->LRU_topology_latch.x_unlock();
  mutex_exit(&buf_pool->LRU_drain_mutex);
}

/** Prints the LRU list. */
void buf_LRU_print(void) {
  for (size_t i = 0; i < srv_buf_pool_instances; i++) {
    buf_pool_t *buf_pool;

    buf_pool = buf_pool_from_array(i);
    buf_LRU_print_instance(buf_pool);
  }
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */
