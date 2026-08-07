/* Copyright (c) 2026, Percona LLC.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA */

#include "ut0bounded_mpsc.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace ut::unittests {

TEST(BoundedGenerationDedup, SuppressesUntilExactGenerationIsReleased) {
  Bounded_generation_dedup dedup{2};

  EXPECT_TRUE(dedup.try_reserve(2));
  EXPECT_FALSE(dedup.try_reserve(2));

  /* A colliding generation cannot replace or clear the pending owner. */
  EXPECT_FALSE(dedup.try_reserve(4));
  EXPECT_FALSE(dedup.release(4));
  EXPECT_FALSE(dedup.try_reserve(4));

  EXPECT_TRUE(dedup.release(2));
  EXPECT_TRUE(dedup.try_reserve(4));
  EXPECT_TRUE(dedup.release(4));
}

TEST(BoundedGenerationDedup, OneConcurrentProducerOwnsGeneration) {
  Bounded_generation_dedup dedup{16};
  std::atomic_uint32_t reserved{0};
  std::vector<std::thread> producers;

  for (uint32_t i = 0; i < 16; ++i) {
    producers.emplace_back([&] {
      if (dedup.try_reserve(1234)) {
        reserved.fetch_add(1);
      }
    });
  }
  for (auto &producer : producers) {
    producer.join();
  }

  EXPECT_EQ(reserved.load(), 1U);
  EXPECT_TRUE(dedup.release(1234));
}

TEST(BoundedMpscQueue, FullAndReuse) {
  Bounded_mpsc_queue<uint32_t> queue{2};

  EXPECT_EQ(queue.try_push(10), Bounded_mpsc_push_result::first);
  EXPECT_EQ(queue.try_push(20), Bounded_mpsc_push_result::pending);
  EXPECT_EQ(queue.try_push(30), Bounded_mpsc_push_result::full);
  EXPECT_EQ(queue.size(), 2U);

  auto value = queue.try_pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 10);
  value = queue.try_pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 20);
  EXPECT_FALSE(queue.try_pop().has_value());
  EXPECT_EQ(queue.size(), 0U);

  EXPECT_TRUE(queue.reset_wakeup_if_empty());
  EXPECT_EQ(queue.try_push(30), Bounded_mpsc_push_result::first);
  value = queue.try_pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 30);
}

TEST(BoundedMpscQueue, ArmsWakeupOnlyAtThreshold) {
  Bounded_mpsc_queue<uint32_t> queue{4};

  EXPECT_EQ(queue.try_push(10, 3), Bounded_mpsc_push_result::pending);
  EXPECT_EQ(queue.try_push(20, 3), Bounded_mpsc_push_result::pending);
  EXPECT_EQ(queue.try_push(30, 3), Bounded_mpsc_push_result::first);
  EXPECT_EQ(queue.try_push(40, 3), Bounded_mpsc_push_result::pending);

  while (queue.try_pop().has_value()) {
  }
  EXPECT_TRUE(queue.reset_wakeup_if_empty());
  EXPECT_EQ(queue.try_push(50, 0), Bounded_mpsc_push_result::first);
}

TEST(BoundedMpscQueue, ReservedProducerDoesNotBlockPublishedWork) {
  Bounded_mpsc_queue<uint32_t> queue{3};
  std::atomic_bool producer_paused{false};
  std::atomic_bool release_producer{false};
  std::atomic_bool pause_once{true};
  std::atomic<Bounded_mpsc_push_result> paused_result{
      Bounded_mpsc_push_result::full};

  queue.set_after_reserve_hook([&] {
    if (pause_once.exchange(false)) {
      producer_paused.store(true);
      while (!release_producer.load()) {
        std::this_thread::yield();
      }
    }
  });

  std::thread paused_producer([&] { paused_result.store(queue.try_push(1)); });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!producer_paused.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  std::optional<uint32_t> value;
  Bounded_mpsc_push_result active_result = Bounded_mpsc_push_result::full;
  if (producer_paused.load()) {
    active_result = queue.try_push(2);
    value = queue.try_pop();
  }

  release_producer.store(true);
  paused_producer.join();

  ASSERT_TRUE(producer_paused.load());
  EXPECT_EQ(active_result, Bounded_mpsc_push_result::first);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 2);
  EXPECT_NE(paused_result.load(), Bounded_mpsc_push_result::full);
  value = queue.try_pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 1);
}

TEST(BoundedMpscQueue, WakeResetRechecksPublishedSlots) {
  Bounded_mpsc_queue<uint32_t> queue{2};

  EXPECT_EQ(queue.try_push(1), Bounded_mpsc_push_result::first);
  EXPECT_EQ(queue.try_push(2), Bounded_mpsc_push_result::pending);
  ASSERT_TRUE(queue.try_pop().has_value());
  EXPECT_FALSE(queue.reset_wakeup_if_empty());
  ASSERT_TRUE(queue.try_pop().has_value());
  EXPECT_TRUE(queue.reset_wakeup_if_empty());
  EXPECT_EQ(queue.try_push(3), Bounded_mpsc_push_result::first);
}

TEST(BoundedMpscQueue, ReadinessChecksDoNotScanCapacity) {
  Bounded_mpsc_queue<uint32_t> queue{4096};

  queue.reset_ready_probe_count();
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.ready_probe_count(), 0U);

  EXPECT_EQ(queue.try_push(17), Bounded_mpsc_push_result::first);
  EXPECT_FALSE(queue.empty());
  EXPECT_EQ(queue.ready_probe_count(), 0U);

  const auto value = queue.try_pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 17U);
  EXPECT_LE(queue.ready_probe_count(), 2U);

  EXPECT_TRUE(queue.empty());
  EXPECT_TRUE(queue.reset_wakeup_if_empty());
  EXPECT_EQ(queue.ready_probe_count(), 2U);
}

TEST(BoundedMpscQueue, ProducerRacingWakeClearCannotLoseWake) {
  Bounded_mpsc_queue<uint32_t> queue{2};
  std::atomic_bool consumer_paused{false};
  std::atomic_bool release_consumer{false};
  std::atomic_bool reset_result{true};

  EXPECT_EQ(queue.try_push(1), Bounded_mpsc_push_result::first);
  ASSERT_TRUE(queue.try_pop().has_value());

  queue.set_before_wakeup_clear_hook([&] {
    consumer_paused.store(true);
    while (!release_consumer.load()) {
      std::this_thread::yield();
    }
  });

  std::thread consumer(
      [&] { reset_result.store(queue.reset_wakeup_if_empty()); });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!consumer_paused.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  Bounded_mpsc_push_result producer_result = Bounded_mpsc_push_result::full;
  if (consumer_paused.load()) {
    producer_result = queue.try_push(2);
  }

  release_consumer.store(true);
  consumer.join();

  ASSERT_TRUE(consumer_paused.load());
  EXPECT_EQ(producer_result, Bounded_mpsc_push_result::pending);
  EXPECT_FALSE(reset_result.load());
  const auto value = queue.try_pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 2);
}

TEST(BoundedMpscQueue, ConcurrentProducersAndConsumer) {
  constexpr uint32_t producer_count = 4;
  constexpr uint32_t values_per_producer = 2000;
  constexpr uint32_t value_count = producer_count * values_per_producer;

  Bounded_mpsc_queue<uint32_t> queue{64};
  std::array<std::atomic_bool, value_count> seen{};
  std::atomic_uint32_t producers_done{0};
  std::atomic_uint32_t consumed{0};
  std::atomic_bool duplicate{false};
  std::atomic_bool out_of_range{false};
  std::atomic_bool stop{false};
  std::atomic_bool timed_out{false};

  for (auto &was_seen : seen) {
    was_seen.store(false);
  }

  std::thread consumer([&] {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (producers_done.load() != producer_count ||
           consumed.load() != value_count) {
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out.store(true);
        stop.store(true);
        break;
      }

      const auto value = queue.try_pop();
      if (!value.has_value()) {
        std::this_thread::yield();
        continue;
      }

      if (*value >= value_count) {
        out_of_range.store(true);
        continue;
      }
      if (seen[*value].exchange(true)) {
        duplicate.store(true);
      }
      consumed.fetch_add(1);
    }
  });

  std::vector<std::thread> producers;
  for (uint32_t producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer] {
      const auto begin = producer * values_per_producer;
      const auto end = begin + values_per_producer;
      for (uint32_t value = begin; value < end; ++value) {
        while (queue.try_push(value) == Bounded_mpsc_push_result::full) {
          if (stop.load()) {
            break;
          }
          std::this_thread::yield();
        }
        if (stop.load()) {
          break;
        }
      }
      producers_done.fetch_add(1);
    });
  }

  for (auto &producer : producers) {
    producer.join();
  }
  consumer.join();

  EXPECT_FALSE(timed_out.load());
  EXPECT_FALSE(out_of_range.load());
  EXPECT_FALSE(duplicate.load());
  EXPECT_EQ(consumed.load(), value_count);
  for (const auto &was_seen : seen) {
    EXPECT_TRUE(was_seen.load());
  }
}

struct Non_default_id {
  explicit Non_default_id(uint32_t space_id, uint32_t page_no)
      : space_id{space_id}, page_no{page_no} {}

  uint32_t space_id;
  uint32_t page_no;
};

struct Promotion_identity {
  Non_default_id page_id;
  uint64_t residency_generation;
};

static_assert(std::is_trivially_copyable_v<Promotion_identity>);
static_assert(!std::is_default_constructible_v<Non_default_id>);

TEST(BoundedMpscQueue, SupportsPromotionIdentityWithoutDefaultConstructor) {
  Bounded_mpsc_queue<Promotion_identity> queue{2};
  const Promotion_identity identity{Non_default_id{42, 17}, 1234};

  EXPECT_EQ(queue.try_push(identity), Bounded_mpsc_push_result::first);
  const auto popped = queue.try_pop();
  ASSERT_TRUE(popped.has_value());
  EXPECT_EQ(popped->page_id.space_id, identity.page_id.space_id);
  EXPECT_EQ(popped->page_id.page_no, identity.page_id.page_no);
  EXPECT_EQ(popped->residency_generation, identity.residency_generation);
}

}  // namespace ut::unittests
