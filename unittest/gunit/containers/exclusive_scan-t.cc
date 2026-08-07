/* Copyright (c) 2026, Percona LLC.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0. */

#include "ut0exclusive_scan.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace ut::unittests {

TEST(ExclusiveScan, TryAcquireIsExclusive) {
  Exclusive_scan owner;

  EXPECT_FALSE(owner.is_owned());
  EXPECT_TRUE(owner.try_acquire());
  EXPECT_TRUE(owner.is_owned());
  EXPECT_FALSE(owner.try_acquire());
  owner.release();
  EXPECT_FALSE(owner.is_owned());
  EXPECT_TRUE(owner.try_acquire());
  owner.release();
}

TEST(ExclusiveScan, AcquireWaitsForCurrentOwner) {
  Exclusive_scan owner;
  ASSERT_TRUE(owner.try_acquire());

  std::atomic_bool started{false};
  std::atomic_bool acquired{false};
  std::thread waiter([&] {
    started.store(true, std::memory_order_release);
    owner.acquire();
    acquired.store(true, std::memory_order_release);
    owner.release();
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!started.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  const bool waiter_started = started.load(std::memory_order_acquire);
  EXPECT_FALSE(acquired.load(std::memory_order_acquire));
  owner.release();
  waiter.join();
  EXPECT_TRUE(waiter_started);
  EXPECT_TRUE(acquired.load(std::memory_order_acquire));
}

}  // namespace ut::unittests
