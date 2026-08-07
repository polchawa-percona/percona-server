/* Copyright (c) 2026, Percona and/or its affiliates.

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

/* Two-level grouped LRU list: topology S/X latch (PS-11141). See
.requirements/20260807T154100Z_lru_two_level_locking/REQUIREMENTS.md,
Implementation Plan step 1. */

#include "buf0lru_topology.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace innodb_buf0lru_topology_unittest {

class BufLRUTopologyLatch : public ::testing::Test {
 protected:
  void SetUp() override {
    os_event_global_init();
    sync_check_init(4);
    latch_.create();
  }

  void TearDown() override {
    latch_.free();
    sync_check_close();
    os_event_global_destroy();
  }

  Buf_LRU_topology_latch latch_;
};

TEST_F(BufLRUTopologyLatch, StartsUnowned) {
  EXPECT_FALSE(latch_.owns_s());
  EXPECT_FALSE(latch_.owns_x());
  EXPECT_FALSE(latch_.owns_s_or_x());
}

TEST_F(BufLRUTopologyLatch, SLockTracksOwnership) {
  latch_.s_lock();
  EXPECT_TRUE(latch_.owns_s());
  EXPECT_FALSE(latch_.owns_x());
  latch_.s_unlock();
  EXPECT_FALSE(latch_.owns_s());
}

TEST_F(BufLRUTopologyLatch, XLockTracksOwnership) {
  latch_.x_lock();
  EXPECT_FALSE(latch_.owns_s());
  EXPECT_TRUE(latch_.owns_x());
  latch_.x_unlock();
  EXPECT_FALSE(latch_.owns_x());
}

TEST_F(BufLRUTopologyLatch, MultipleReadersConcurrent) {
  std::atomic_bool second_acquired{false};

  latch_.s_lock();

  std::thread reader([&] {
    latch_.s_lock();
    second_acquired.store(true, std::memory_order_release);
    latch_.s_unlock();
  });

  reader.join();

  EXPECT_TRUE(second_acquired.load(std::memory_order_acquire));
  latch_.s_unlock();
}

TEST_F(BufLRUTopologyLatch, XExcludesX) {
  latch_.x_lock();

  std::atomic_bool started{false};
  std::atomic_bool acquired{false};
  std::thread writer([&] {
    started.store(true, std::memory_order_release);
    latch_.x_lock();
    acquired.store(true, std::memory_order_release);
    latch_.x_unlock();
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!started.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  /* Give the second thread a real chance to (wrongly) acquire X while the
  first thread still holds it. */
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  EXPECT_FALSE(acquired.load(std::memory_order_acquire));

  latch_.x_unlock();
  writer.join();
  EXPECT_TRUE(acquired.load(std::memory_order_acquire));
}

TEST_F(BufLRUTopologyLatch, XExcludesS) {
  latch_.x_lock();

  std::atomic_bool started{false};
  std::atomic_bool acquired{false};
  std::thread reader([&] {
    started.store(true, std::memory_order_release);
    latch_.s_lock();
    acquired.store(true, std::memory_order_release);
    latch_.s_unlock();
  });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!started.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  EXPECT_FALSE(acquired.load(std::memory_order_acquire));

  latch_.x_unlock();
  reader.join();
  EXPECT_TRUE(acquired.load(std::memory_order_acquire));
}

}  // namespace innodb_buf0lru_topology_unittest
