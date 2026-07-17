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
 *acquire* a mutex; neither records how long a latch is *held*.  This small,
 self-contained registry fills that gap for a single latch of interest, the
 buffer pool LRU list mutex, so we can measure per acquisition call-site how
 many times the latch is taken and how long it is held.

 Collection is gated by the existing InnoDB latch monitor
 (innodb_monitor_enable='latch'); when it is off, the only overhead added to
 every other mutex is a single boolean test in GenericPolicy::locked/release.

 Results are summed across all buffer pool instances and reported by
 SHOW ENGINE INNODB MUTEX.
 *******************************************************/

#ifndef sync0lru_hold_h
#define sync0lru_hold_h

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
    /** Sum of hold durations, in nanoseconds. */
    std::atomic<uint64_t> m_total_ns{0};
    /** Longest single hold, in nanoseconds. */
    std::atomic<uint64_t> m_max_ns{0};
  };

  Slot m_slots[MAX_SLOTS];

  /** Set when more than MAX_SLOTS distinct call-sites were seen and some
  samples had to be dropped. */
  std::atomic<bool> m_overflow{false};

  /** @return a monotonic timestamp in nanoseconds. */
  static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  /** Record one hold.
  @param[in]  file     source file of the acquisition call-site
  @param[in]  line     source line of the acquisition call-site
  @param[in]  hold_ns  hold duration in nanoseconds */
  void record(const char *file, unsigned line, uint64_t hold_ns) {
    Slot *s = find_or_claim(file, line);

    if (s == nullptr) {
      m_overflow.store(true, std::memory_order_relaxed);
      return;
    }

    s->m_count.fetch_add(1, std::memory_order_relaxed);
    s->m_total_ns.fetch_add(hold_ns, std::memory_order_relaxed);

    uint64_t cur = s->m_max_ns.load(std::memory_order_relaxed);
    while (hold_ns > cur && !s->m_max_ns.compare_exchange_weak(
                                cur, hold_ns, std::memory_order_relaxed)) {
    }
  }

  /** Zero all accumulated statistics, keeping the call-site claims so that
  slots do not have to be re-claimed (avoids a claim race on reset). */
  void reset() {
    for (Slot &s : m_slots) {
      s.m_count.store(0, std::memory_order_relaxed);
      s.m_total_ns.store(0, std::memory_order_relaxed);
      s.m_max_ns.store(0, std::memory_order_relaxed);
    }
    m_overflow.store(false, std::memory_order_relaxed);
  }

  /** Invoke f(file, line, count, total_ns, max_ns) for every claimed slot
  that has at least one sample. */
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
        s.m_total_ns.load(std::memory_order_relaxed),
        s.m_max_ns.load(std::memory_order_relaxed));
    }
  }

  /** @return true if any samples were dropped due to slot exhaustion. */
  bool overflowed() const { return m_overflow.load(std::memory_order_relaxed); }

 private:
  /** Locate the slot for (file, line), claiming a free slot on first use.
  @return the slot, or nullptr if all slots are exhausted. */
  Slot *find_or_claim(const char *file, unsigned line) {
    for (Slot &s : m_slots) {
      const char *cur = s.m_file.load(std::memory_order_acquire);

      if (cur == nullptr) {
        /* Free slot: try to claim it.  Publish the line first, then the
        file pointer as the release marker so a concurrent reader that sees
        our file also sees the matching line. */
        s.m_line.store(line, std::memory_order_relaxed);
        const char *expected = nullptr;
        if (s.m_file.compare_exchange_strong(expected, file,
                                             std::memory_order_release,
                                             std::memory_order_acquire)) {
          return &s;
        }
        /* Lost the race: expected now holds whoever won.  Fall through to
        the match check below against the winner's key. */
        cur = expected;
      }

      if ((cur == file || std::strcmp(cur, file) == 0) &&
          s.m_line.load(std::memory_order_relaxed) == line) {
        return &s;
      }
    }
    return nullptr;
  }
};

/** The single global registry.  Trivially destructible, so it is safe to use
as an inline global regardless of static-destruction order at shutdown. */
inline LruHoldStats lru_hold_stats;

#endif /* sync0lru_hold_h */
