/*****************************************************************************

Copyright (c) 2026, Percona LLC.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/

#ifndef ut0bounded_mpsc_h
#define ut0bounded_mpsc_h

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
/* Keep the standalone gunit target free of the InnoDB sync/include tree. */
constexpr size_t BOUNDED_MPSC_CACHE_LINE_SIZE = 64;
#else
#include "ut0cpu_cache.h"
constexpr size_t BOUNDED_MPSC_CACHE_LINE_SIZE = ut::INNODB_CACHE_LINE_SIZE;
#endif

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
#include <functional>
#endif

namespace ut {

/** Result of a bounded MPSC enqueue attempt. */
enum class Bounded_mpsc_push_result {
  /** No free preallocated node was available. */
  full,
  /** This producer armed the consumer wakeup. */
  first,
  /** The item was published without arming a wakeup. */
  pending
};

/** A preallocated multiple-producer, single-consumer value container.

Each producer selects one slot using a monotonically increasing ticket and
atomically changes that slot from free to reserved. A busy selected slot makes
the bounded heuristic enqueue fail immediately; producers never search or
wait. A producer preempted while owning one reserved slot cannot prevent the
consumer from processing other ready slots. Nodes are not linked and no
operation retains a structural observation across slot reuse, so correctness
does not depend on a finite ABA tag.

@tparam T trivially copyable value stored in each node */
template <typename T>
class Bounded_mpsc_queue {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  /** Construct an empty queue with all storage allocated up front.
  @param capacity maximum number of simultaneously reserved/pending values */
  explicit Bounded_mpsc_queue(uint32_t capacity)
      : m_slots{std::make_unique<Slot[]>(capacity)}, m_capacity{capacity} {
    if (capacity == 0) {
      std::abort();
    }
  }

  ~Bounded_mpsc_queue() = default;

  Bounded_mpsc_queue(const Bounded_mpsc_queue &) = delete;
  Bounded_mpsc_queue(Bounded_mpsc_queue &&) = delete;
  Bounded_mpsc_queue &operator=(const Bounded_mpsc_queue &) = delete;
  Bounded_mpsc_queue &operator=(Bounded_mpsc_queue &&) = delete;

  /** Try to enqueue a copied value without allocating or taking a mutex.
  @param value value to copy
  @param wake_threshold pending count at which one producer arms a wakeup;
  zero is normalized to one
  @return full, wakeup owner, or published without wakeup */
  [[nodiscard]] Bounded_mpsc_push_result try_push(
      const T &value, uint32_t wake_threshold = 1) {
    const uint64_t ticket =
        m_next_ticket.value.fetch_add(1, std::memory_order_relaxed);
    Slot &slot = m_slots[ticket % m_capacity];
    State expected = State::free;
    if (!slot.state.compare_exchange_strong(expected, State::reserved,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
      return Bounded_mpsc_push_result::full;
    }

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
    if (m_after_reserve_hook) {
      m_after_reserve_hook();
    }
#endif

    slot.value.emplace(value);
    const uint32_t previous =
        m_pending_count.value.fetch_add(1, std::memory_order_relaxed);
    slot.state.store(State::ready, std::memory_order_release);

    wake_threshold = std::max(uint32_t{1}, wake_threshold);
    if (previous + 1 < wake_threshold) {
      return Bounded_mpsc_push_result::pending;
    }

    return m_wakeup_pending.value.exchange(true, std::memory_order_acq_rel)
               ? Bounded_mpsc_push_result::pending
               : Bounded_mpsc_push_result::first;
  }

  /** Try to remove one pending value. Must have only one concurrent caller.
  @return copied value, or nullopt if a complete scan found no ready slot */
  [[nodiscard]] std::optional<T> try_pop() {
    for (uint32_t scanned = 0; scanned < m_capacity; ++scanned) {
      const uint32_t slot_index = (m_consumer_cursor + scanned) % m_capacity;
      Slot &slot = m_slots[slot_index];
      if (slot.state.load(std::memory_order_acquire) != State::ready) {
        continue;
      }

      std::optional<T> value{*slot.value};
      slot.value.reset();
      slot.state.store(State::free, std::memory_order_release);
      const uint32_t previous =
          m_pending_count.value.fetch_sub(1, std::memory_order_relaxed);
      if (previous == 0) {
        std::abort();
      }
      m_consumer_cursor = (slot_index + 1) % m_capacity;
      return value;
    }

    return std::nullopt;
  }

  /** @return maximum simultaneously reserved/pending values */
  [[nodiscard]] uint32_t capacity() const { return m_capacity; }

  /** @return published or publishing values, for wakeup heuristics */
  [[nodiscard]] uint32_t size() const {
    return m_pending_count.value.load(std::memory_order_relaxed);
  }

  /** Complete the consumer's drain-and-sleep handshake.

  Clear the producer wake state, then scan after the clear. If work raced with
  the clear, re-arm the state and tell the consumer to keep draining. If work
  arrives after the scan, that producer observes false and signals.
  @return true if the consumer may sleep */
  [[nodiscard]] bool reset_wakeup_if_empty() {
#ifdef EXTRA_CODE_FOR_UNIT_TESTING
    if (m_before_wakeup_clear_hook) {
      m_before_wakeup_clear_hook();
    }
#endif

    static_cast<void>(
        m_wakeup_pending.value.exchange(false, std::memory_order_acq_rel));
    if (!empty()) {
      m_wakeup_pending.value.store(true, std::memory_order_release);
      return false;
    }
    return true;
  }

  /** @return true when a complete scan finds no published value */
  [[nodiscard]] bool empty() const {
    for (uint32_t i = 0; i < m_capacity; ++i) {
      if (m_slots[i].state.load(std::memory_order_acquire) == State::ready) {
        return false;
      }
    }
    return true;
  }

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
  /** Install a hook run after reservation and before publication. */
  void set_after_reserve_hook(std::function<void()> hook) {
    m_after_reserve_hook = std::move(hook);
  }

  /** Install a hook run before the consumer clears the wake state. */
  void set_before_wakeup_clear_hook(std::function<void()> hook) {
    m_before_wakeup_clear_hook = std::move(hook);
  }

#endif

 private:
  enum class State : uint8_t { free, reserved, ready };

  struct Slot {
    std::optional<T> value;
    std::atomic<State> state{State::free};
  };

  static_assert(std::atomic<State>::is_always_lock_free);

  template <typename U>
  struct alignas(BOUNDED_MPSC_CACHE_LINE_SIZE) Cacheline_atomic {
    std::atomic<U> value{};
  };

  static_assert(
      decltype(Cacheline_atomic<uint64_t>::value)::is_always_lock_free);
  static_assert(
      decltype(Cacheline_atomic<uint32_t>::value)::is_always_lock_free);
  static_assert(decltype(Cacheline_atomic<bool>::value)::is_always_lock_free);
  static_assert(sizeof(Cacheline_atomic<uint64_t>) ==
                BOUNDED_MPSC_CACHE_LINE_SIZE);
  static_assert(sizeof(Cacheline_atomic<bool>) == BOUNDED_MPSC_CACHE_LINE_SIZE);

  std::unique_ptr<Slot[]> m_slots;
  const uint32_t m_capacity;
  uint32_t m_consumer_cursor{};
  Cacheline_atomic<uint64_t> m_next_ticket;
  Cacheline_atomic<uint32_t> m_pending_count;
  Cacheline_atomic<bool> m_wakeup_pending;

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
  std::function<void()> m_after_reserve_hook;
  std::function<void()> m_before_wakeup_clear_hook;
#endif
};

}  // namespace ut

#endif /* ut0bounded_mpsc_h */
