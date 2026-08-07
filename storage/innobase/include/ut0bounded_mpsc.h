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
#include <array>
#include <atomic>
#include <bit>
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

/** Fixed-size producer-side duplicate suppression keyed by a nonzero,
pool-unique generation. A collision drops the heuristic request rather than
making a producer search or wait. */
class Bounded_generation_dedup {
 public:
  explicit Bounded_generation_dedup(uint32_t capacity)
      : m_tags{std::make_unique<std::atomic<uint64_t>[]>(capacity)},
        m_capacity{capacity} {
    if (capacity == 0) {
      std::abort();
    }
    for (uint32_t i = 0; i < capacity; ++i) {
      m_tags[i].store(0, std::memory_order_relaxed);
    }
  }

  Bounded_generation_dedup(const Bounded_generation_dedup &) = delete;
  Bounded_generation_dedup(Bounded_generation_dedup &&) = delete;
  Bounded_generation_dedup &operator=(const Bounded_generation_dedup &) =
      delete;
  Bounded_generation_dedup &operator=(Bounded_generation_dedup &&) = delete;

  /** Reserve the generation's deterministic slot.
  @return true only when this caller installed the generation */
  [[nodiscard]] bool try_reserve(uint64_t generation) {
    if (generation == 0) {
      return false;
    }
    auto &tag = m_tags[generation % m_capacity];
    uint64_t expected = 0;
    return tag.compare_exchange_strong(expected, generation,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
  }

  /** Release a slot only if it still contains the exact generation.
  @return true if the generation was released */
  [[nodiscard]] bool release(uint64_t generation) {
    if (generation == 0) {
      return false;
    }
    auto &tag = m_tags[generation % m_capacity];
    return tag.compare_exchange_strong(generation, 0, std::memory_order_acq_rel,
                                       std::memory_order_acquire);
  }

 private:
  std::unique_ptr<std::atomic<uint64_t>[]> m_tags;
  const uint32_t m_capacity;
};

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
    if (capacity == 0 || capacity > MAX_CAPACITY) {
      std::abort();
    }
    for (auto &word : m_ready_words) {
      word.store(0, std::memory_order_relaxed);
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
    publish_ready(static_cast<uint32_t>(ticket % m_capacity));

    wake_threshold = std::max(uint32_t{1}, wake_threshold);
    if (previous + 1 < wake_threshold) {
      return Bounded_mpsc_push_result::pending;
    }

    return m_wakeup_pending.value.exchange(true, std::memory_order_acq_rel)
               ? Bounded_mpsc_push_result::pending
               : Bounded_mpsc_push_result::first;
  }

  /** Try to remove one pending value. Must have only one concurrent caller.
  A two-level ready bitmap locates a published slot without scanning queue
  capacity.
  @return copied value, or nullopt if no slot is currently published */
  [[nodiscard]] std::optional<T> try_pop() {
    for (;;) {
#ifdef EXTRA_CODE_FOR_UNIT_TESTING
      ++m_ready_probe_count;
#endif
      const uint64_t summary =
          m_ready_summary.value.load(std::memory_order_acquire);
      if (summary == 0) {
        return std::nullopt;
      }

      const uint32_t word_index = std::countr_zero(summary);
      const uint64_t summary_bit = uint64_t{1} << word_index;
#ifdef EXTRA_CODE_FOR_UNIT_TESTING
      ++m_ready_probe_count;
#endif
      const uint64_t ready =
          m_ready_words[word_index].load(std::memory_order_acquire);
      if (ready == 0) {
        clear_summary_bit(word_index, summary_bit);
        continue;
      }

      const uint32_t bit_index = std::countr_zero(ready);
      const uint64_t ready_bit = uint64_t{1} << bit_index;
      const uint32_t slot_index = word_index * READY_WORD_BITS + bit_index;
      const uint64_t previous = m_ready_words[word_index].fetch_and(
          ~ready_bit, std::memory_order_acq_rel);
      if ((previous & ready_bit) == 0) {
        continue;
      }
      if ((previous & ~ready_bit) == 0) {
        clear_summary_bit(word_index, summary_bit);
      }

      Slot &slot = m_slots[slot_index];
      if (slot.state.load(std::memory_order_acquire) != State::ready) {
        std::abort();
      }
      std::optional<T> value{*slot.value};
      slot.value.reset();
      slot.state.store(State::free, std::memory_order_release);
      const uint32_t pending_before =
          m_pending_count.value.fetch_sub(1, std::memory_order_relaxed);
      if (pending_before == 0) {
        std::abort();
      }
      return value;
    }
  }

  /** @return maximum simultaneously reserved/pending values */
  [[nodiscard]] uint32_t capacity() const { return m_capacity; }

  /** @return published or publishing values, for wakeup heuristics */
  [[nodiscard]] uint32_t size() const {
    return m_pending_count.value.load(std::memory_order_relaxed);
  }

  /** Complete the consumer's drain-and-sleep handshake.

  Clear the producer wake state, then recheck the O(1) pending count. If work
  raced with the clear, re-arm the state and tell the consumer to keep
  draining. If work arrives after the recheck, that producer observes false
  and signals.
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

  /** @return true when no value is published or being published */
  [[nodiscard]] bool empty() const {
    return m_pending_count.value.load(std::memory_order_acquire) == 0;
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

  /** Reset the number of ready-bitmap probes made by try_pop(). */
  void reset_ready_probe_count() { m_ready_probe_count = 0; }

  /** @return number of ready-bitmap probes made by try_pop(). */
  [[nodiscard]] uint32_t ready_probe_count() const {
    return m_ready_probe_count;
  }

#endif

 private:
  static constexpr uint32_t READY_WORD_BITS = 64;
  static constexpr uint32_t READY_SUMMARY_BITS = 64;
  static constexpr uint32_t MAX_CAPACITY = READY_WORD_BITS * READY_SUMMARY_BITS;

  enum class State : uint8_t { free, reserved, ready };

  struct Slot {
    std::optional<T> value;
    std::atomic<State> state{State::free};
  };

  static_assert(std::atomic<State>::is_always_lock_free);

  /** Publish one ready slot into the two-level consumer bitmap. */
  void publish_ready(uint32_t slot_index) {
    const uint32_t word_index = slot_index / READY_WORD_BITS;
    const uint32_t bit_index = slot_index % READY_WORD_BITS;
    m_ready_words[word_index].fetch_or(uint64_t{1} << bit_index,
                                       std::memory_order_release);
    m_ready_summary.value.fetch_or(uint64_t{1} << word_index,
                                   std::memory_order_release);
  }

  /** Clear a summary bit and restore it if a producer raced the clear. */
  void clear_summary_bit(uint32_t word_index, uint64_t summary_bit) {
    m_ready_summary.value.fetch_and(~summary_bit, std::memory_order_acq_rel);
    if (m_ready_words[word_index].load(std::memory_order_acquire) != 0) {
      m_ready_summary.value.fetch_or(summary_bit, std::memory_order_release);
    }
  }

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
  std::array<std::atomic<uint64_t>, READY_SUMMARY_BITS> m_ready_words{};
  Cacheline_atomic<uint64_t> m_next_ticket;
  Cacheline_atomic<uint32_t> m_pending_count;
  Cacheline_atomic<bool> m_wakeup_pending;
  Cacheline_atomic<uint64_t> m_ready_summary;

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
  std::function<void()> m_after_reserve_hook;
  std::function<void()> m_before_wakeup_clear_hook;
  uint32_t m_ready_probe_count{};
#endif
};

}  // namespace ut

#endif /* ut0bounded_mpsc_h */
