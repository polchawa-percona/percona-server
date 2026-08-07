/* Copyright (c) 2026, Percona LLC.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0. */

#ifndef ut0exclusive_scan_h
#define ut0exclusive_scan_h

#include <atomic>
#include <cstdlib>

namespace ut {

/** Single-owner token with nonblocking and waiting acquisition modes. */
class Exclusive_scan {
 public:
  Exclusive_scan() = default;
  ~Exclusive_scan() = default;

  Exclusive_scan(const Exclusive_scan &) = delete;
  Exclusive_scan &operator=(const Exclusive_scan &) = delete;
  Exclusive_scan(Exclusive_scan &&) = delete;
  Exclusive_scan &operator=(Exclusive_scan &&) = delete;

  /** Try to become the sole owner without waiting. */
  [[nodiscard]] bool try_acquire() noexcept {
    bool expected = false;
    return m_owned.compare_exchange_strong(
        expected, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  /** Wait until this thread becomes the sole owner. */
  void acquire() noexcept {
    while (!try_acquire()) {
      m_owned.wait(true, std::memory_order_relaxed);
    }
  }

  /** Return whether a scan currently owns the token. */
  [[nodiscard]] bool is_owned() const noexcept {
    return m_owned.load(std::memory_order_acquire);
  }

  /** Release ownership. */
  void release() noexcept {
    const bool was_owned = m_owned.exchange(false, std::memory_order_release);
    if (!was_owned) {
      std::abort();
    }
    m_owned.notify_one();
  }

 private:
  std::atomic_bool m_owned{false};
};

}  // namespace ut

#endif /* ut0exclusive_scan_h */
