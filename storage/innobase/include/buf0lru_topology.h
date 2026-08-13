/*****************************************************************************

Copyright (c) 2026, Percona and/or its affiliates.

*****************************************************************************/

/** @file include/buf0lru_topology.h
 Two-level grouped LRU list: topology S/X latch (PS-11141).

 Introduced additively per
 .requirements/20260807T154100Z_lru_two_level_locking/REQUIREMENTS.md,
 Implementation Plan step 1: this type is not yet wired into any real
 critical section. buf_pool_t::LRU_topology_latch remains the live protection
 for group topology, slots, occupancy, and lifetime until step 2 migrates
 its call sites onto this latch. */

#ifndef buf0lru_topology_h
#define buf0lru_topology_h

#include "sync0rw.h"
#include "sync0sync.h"
#include "sync0types.h"
#include "ut0new.h"

/** Topology S/X latch for the two-level grouped LRU list (PS-11141).

Wraps an InnoDB rw_lock_t at latch level SYNC_BUF_LRU_LIST (the same level
buf_pool_t::LRU_topology_latch occupies today) and enforces the non-recursive
acquisition rules from the two-level locking REQUIREMENTS.md, Requirement
1: every acquisition of either mode must start from "this thread owns
neither S nor X". S->S, X->X, X->S, S->X upgrade, and any hidden recursive
helper acquisition are all forbidden -- there is deliberately no upgrade
method. A caller that needs X after S must release S first and then
unconditionally acquire X (see Requirement 7's release-and-retry
rollover protocol), never call an upgrade. */
class Buf_LRU_topology_latch {
 public:
  Buf_LRU_topology_latch() = default;

  Buf_LRU_topology_latch(const Buf_LRU_topology_latch &) = delete;
  Buf_LRU_topology_latch &operator=(const Buf_LRU_topology_latch &) = delete;

  /** Creates the underlying rw-lock. Must be called exactly once before
  any other member function.

  rw_lock_t has a non-trivial destructor that rw_lock_free() invokes
  manually (it is placement-constructed by rw_lock_create() over raw
  storage -- see the "We did an in-place new" comment on
  rw_lock_free_func()), so it must never also be a plain, automatically
  constructed/destructed C++ member: that would run its destructor a
  second time (once via free() below, once implicitly when this object
  is destroyed) and crash on the resulting double-teardown. Give it its
  own raw, manually managed storage instead, exactly like every other
  rw_lock_t* in InnoDB. */
  void create() {
    ut_ad(m_lock == nullptr);
    m_lock = static_cast<rw_lock_t *>(
        ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, sizeof(rw_lock_t)));
    rw_lock_create(buf_pool_lru_topology_lock_key, m_lock,
                   LATCH_ID_BUF_POOL_LRU_TOPOLOGY);
  }

  /** Frees the underlying rw-lock. The caller must not hold it in either
  mode and no other thread must be waiting on it. */
  void free() {
    ut_ad(m_lock != nullptr);
    rw_lock_free(m_lock);
    ut::free(m_lock);
    m_lock = nullptr;
  }

  /** Acquires the latch in shared mode.
  @pre the calling thread owns neither S nor X on this latch. */
  void s_lock() {
    ut_ad(!owns_s());
    ut_ad(!owns_x());
    rw_lock_s_lock(m_lock, UT_LOCATION_HERE);
  }

  /** Releases a shared-mode acquisition.
  @pre the calling thread owns S on this latch. */
  void s_unlock() {
    ut_ad(owns_s());
    rw_lock_s_unlock(m_lock);
  }

  /** Acquires the latch in exclusive mode.
  @pre the calling thread owns neither S nor X on this latch. */
  void x_lock() {
    ut_ad(!owns_s());
    ut_ad(!owns_x());
    rw_lock_x_lock(m_lock, UT_LOCATION_HERE);
  }

  /** Releases an exclusive-mode acquisition.
  @pre the calling thread owns X on this latch. */
  void x_unlock() {
    ut_ad(owns_x());
    rw_lock_x_unlock(m_lock);
  }

  /** @return true if the calling thread owns this latch in shared mode. */
  bool owns_s() const { return rw_lock_own(m_lock, RW_LOCK_S); }

  /** @return true if the calling thread owns this latch in exclusive
  mode. */
  bool owns_x() const { return rw_lock_own(m_lock, RW_LOCK_X); }

  /** @return true if the calling thread owns this latch in either mode. */
  bool owns_s_or_x() const { return owns_s() || owns_x(); }

  /** Asserts the calling thread owns this latch in shared mode. */
  void assert_s_locked() const { ut_ad(owns_s()); }

  /** Asserts the calling thread owns this latch in exclusive mode. */
  void assert_x_locked() const { ut_ad(owns_x()); }

  /** Asserts the calling thread owns this latch in either mode. */
  void assert_s_or_x_locked() const { ut_ad(owns_s_or_x()); }

  /** Asserts the calling thread owns this latch in neither mode. */
  void assert_not_locked() const { ut_ad(!owns_s_or_x()); }

 private:
  rw_lock_t *m_lock{nullptr};
};

#endif /* buf0lru_topology_h */
