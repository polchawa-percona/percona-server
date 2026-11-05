/*****************************************************************************

Copyright (c) 2026, Percona Inc. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
either included with the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/sync0lru_hold.h
 Per-call-site hold-time statistics for buf_pool->LRU_list_mutex.

 Performance Schema and the InnoDB latch monitor both time only the wait to
 *acquire* a mutex; neither records how long a latch is *held*, and neither
 attributes the wait to the call-site that acquired the latch.  This small,
 self-contained registry fills that gap for a single latch of interest, the
 buffer pool LRU list mutex, so we can measure per acquisition call-site how
 many times the latch is taken, how long threads waited to get it and how
 long it is held.

 Collection is always on for the LRU list mutex; the only overhead added to
 every other mutex is a single boolean test in GenericPolicy::enter, locked
 and release.  innodb_monitor_reset='latch' clears the statistics.

 Results are summed across all buffer pool instances and reported by
 SHOW ENGINE INNODB MUTEX.  A background thread (lru_hold_stats_thread) also
 appends a snapshot of all call-sites with samples to
 lru_mutex_contention_stats.csv in the data directory once a second.
 *******************************************************/

#ifndef sync0lru_hold_h
#define sync0lru_hold_h

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#else
#include "my_rdtsc.h"
#endif

/** Aggregates LRU_list_mutex hold times, keyed by acquisition call-site
(source file + line).  All members use relaxed atomics: this is measurement
instrumentation, not something correctness depends on. */
struct LruHoldStats {
  /** Number of distinct call-site slots.  There are 48 known acquisition
  sites today; 64 leaves head-room and keeps the linear scan tiny. */
  static constexpr size_t MAX_SLOTS = 64;

  /** One accumulator per acquisition call-site. */
  struct Slot {
    /** Source file of the call-site; also the "claimed" marker for the slot
    (nullptr == free).  Published with release ordering. */
    std::atomic<const char *> m_file{nullptr};
    /** Source line of the call-site. */
    std::atomic<unsigned> m_line{0};
    /** Number of times the latch was released from this call-site. */
    std::atomic<uint64_t> m_count{0};
    /** Sum of hold durations, in timer cycles. */
    std::atomic<uint64_t> m_total_cycles{0};
    /** Longest single hold, in timer cycles. */
    std::atomic<uint64_t> m_max_cycles{0};
    /** Sum of the times spent waiting to acquire the latch, in timer
    cycles. */
    std::atomic<uint64_t> m_wait_total_cycles{0};
    /** Longest single wait to acquire the latch, in timer cycles. */
    std::atomic<uint64_t> m_wait_max_cycles{0};
  };

  Slot m_slots[MAX_SLOTS];

  /** Set when more than MAX_SLOTS distinct call-sites were seen and some
  samples had to be dropped. */
  std::atomic<bool> m_overflow{false};

  /** @return the current value of the CPU timestamp counter.  Reading it is
  several times cheaper than std::chrono::steady_clock::now(), which matters
  because this runs three times per LRU list mutex acquisition.  Durations
  are kept in cycles and converted to nanoseconds only when reported, using
  the rate measured by calibrate() at startup. */
  static uint64_t now_cycles() {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return my_timer_cycles();
#endif
  }

  /** Measure the timer rate against std::chrono::steady_clock over a short
  window and store it for cycles_to_ns().  The rate reported by
  my_timer_info() cannot be used: it is hard-coded to 1e9 for the cycle
  timer. */
  static void calibrate() {
    const uint64_t c0 = now_cycles();
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const uint64_t c1 = now_cycles();
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (c1 > c0) {
      s_ns_per_cycle.store(ns / static_cast<double>(c1 - c0),
                           std::memory_order_relaxed);
    }
  }

  /** @return the duration in nanoseconds, or 0 before calibrate() ran. */
  static uint64_t cycles_to_ns(uint64_t cycles) {
    return static_cast<uint64_t>(
        static_cast<double>(cycles) *
        s_ns_per_cycle.load(std::memory_order_relaxed));
  }

  /** Record one hold.
  @param[in]  file     source file of the acquisition call-site
  @param[in]  line     source line of the acquisition call-site
  @param[in]  hold_cycles  hold duration in timer cycles */
  void record(const char *file, unsigned line, uint64_t hold_cycles) {
    Slot *s = find_or_claim(file, line);

    if (s == nullptr) {
      m_overflow.store(true, std::memory_order_relaxed);
      return;
    }

    s->m_count.fetch_add(1, std::memory_order_relaxed);
    s->m_total_cycles.fetch_add(hold_cycles, std::memory_order_relaxed);
    update_max(s->m_max_cycles, hold_cycles);
  }

  /** Record one wait to acquire the latch.  The acquisition itself is counted
  by record() when the latch is released.
  @param[in]  file     source file of the acquisition call-site
  @param[in]  line     source line of the acquisition call-site
  @param[in]  wait_cycles  time spent waiting for the latch, in timer
                           cycles */
  void record_wait(const char *file, unsigned line, uint64_t wait_cycles) {
    Slot *s = find_or_claim(file, line);

    if (s == nullptr) {
      m_overflow.store(true, std::memory_order_relaxed);
      return;
    }

    s->m_wait_total_cycles.fetch_add(wait_cycles, std::memory_order_relaxed);
    update_max(s->m_wait_max_cycles, wait_cycles);
  }

  /** Zero all accumulated statistics, keeping the call-site claims so that
  slots do not have to be re-claimed (avoids a claim race on reset). */
  void reset() {
    for (Slot &s : m_slots) {
      s.m_count.store(0, std::memory_order_relaxed);
      s.m_total_cycles.store(0, std::memory_order_relaxed);
      s.m_max_cycles.store(0, std::memory_order_relaxed);
      s.m_wait_total_cycles.store(0, std::memory_order_relaxed);
      s.m_wait_max_cycles.store(0, std::memory_order_relaxed);
    }
    m_overflow.store(false, std::memory_order_relaxed);
  }

  /** Invoke f(file, line, count, total_ns, max_ns, wait_total_ns,
  wait_max_ns) for every claimed slot that has at least one sample.  The
  durations are converted from timer cycles to nanoseconds. */
  template <typename F>
  void iterate(F &&f) const {
    for (const Slot &s : m_slots) {
      const char *file = s.m_file.load(std::memory_order_acquire);
      if (file == nullptr) {
        continue;
      }
      uint64_t count = s.m_count.load(std::memory_order_relaxed);
      if (count == 0) {
        continue;
      }
      f(file, s.m_line.load(std::memory_order_relaxed), count,
        cycles_to_ns(s.m_total_cycles.load(std::memory_order_relaxed)),
        cycles_to_ns(s.m_max_cycles.load(std::memory_order_relaxed)),
        cycles_to_ns(s.m_wait_total_cycles.load(std::memory_order_relaxed)),
        cycles_to_ns(s.m_wait_max_cycles.load(std::memory_order_relaxed)));
    }
  }

  /** @return true if any samples were dropped due to slot exhaustion. */
  bool overflowed() const { return m_overflow.load(std::memory_order_relaxed); }

 private:
  /** Nanoseconds per timer cycle, set by calibrate(). */
  static inline std::atomic<double> s_ns_per_cycle{0.0};

  /** Raise max to value if value is larger. */
  static void update_max(std::atomic<uint64_t> &max, uint64_t value) {
    uint64_t cur = max.load(std::memory_order_relaxed);
    while (value > cur &&
           !max.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
  }

  /** Locate the slot for (file, line), claiming a free slot on first use.

  The key is the pair (file pointer, line).  Each acquisition call-site has a
  single, stable __FILE__ pointer (captured by UT_LOCATION_HERE at the call
  site), so a plain pointer comparison identifies the file -- no strcmp is
  needed, and it keeps this in-lock path cheap.

  Claim protocol: publish the file pointer with a CAS first, then store the
  line.  m_line is 0 in the tiny window between those two steps; because a
  real __LINE__ is never 0, a reader that sees our file with m_line == 0
  knows a claim is in progress and waits for the line to appear.  Storing the
  line before the CAS (as an earlier version did) is racy: a thread that then
  loses the CAS would have already clobbered the winner's line, corrupting
  per-call-site attribution.
  @return the slot, or nullptr if all slots are exhausted. */
  Slot *find_or_claim(const char *file, unsigned line) {
    for (Slot &s : m_slots) {
      const char *cur = s.m_file.load(std::memory_order_acquire);

      if (cur == nullptr) {
        /* Free slot: try to claim it. */
        const char *expected = nullptr;
        if (s.m_file.compare_exchange_strong(expected, file,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
          /* We own the slot; publish the line only now. */
          s.m_line.store(line, std::memory_order_release);
          return &s;
        }
        /* Lost the race: expected now holds the winner's file pointer. */
        cur = expected;
      }

      if (cur == file) {
        /* Same file.  Wait for the claimer to publish a non-zero line, then
        check whether this slot is for our line or a different call-site in
        the same file. */
        unsigned l;
        while ((l = s.m_line.load(std::memory_order_acquire)) == 0) {
        }
        if (l == line) {
          return &s;
        }
      }
    }
    return nullptr;
  }
};

/** The single global registry.  Trivially destructible, so it is safe to use
as an inline global regardless of static-destruction order at shutdown. */
inline LruHoldStats lru_hold_stats;

/** Timer cycle at which the current thread started waiting for an LRU list
mutex, or 0 when it is not waiting for one.  Set in
GenericPolicy::enter() and consumed in GenericPolicy::locked().  Waiters of the
same mutex run concurrently, so this cannot live in the mutex policy. */
inline thread_local uint64_t lru_wait_start_cycles = 0;

/** Background thread that appends a snapshot of lru_hold_stats to
lru_mutex_contention_stats.csv in the data directory once a second. */
void lru_hold_stats_thread();

/** Event used to wake up lru_hold_stats_thread at shutdown. */
struct os_event;
extern os_event *srv_lru_hold_stats_event;

#endif /* sync0lru_hold_h */
