/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/**
  @file storage/perfschema/pfs_latch_source.cc
  Percona: wait and hold statistics of mutexes and rwlocks aggregated per
  acquire source line (implementation).

  Rows live in a fixed size, insert-only open addressing table. A slot is
  claimed with a compare and swap on its state, so lookups and inserts are
  lock free and need no LF_HASH pins.

  The acquire source is only known when a wait starts, while the wait time is
  only known when it ends, so each thread remembers its pending waits, keyed
  by the locker state, in a small thread local array. Hold time is measured
  from the end of a successful wait to the matching unlock, using a bounded
  thread local stack of held locks. Both thread local structures remember the
  table generation, so rows freed by TRUNCATE TABLE are never updated through
  a stale pointer.
*/

#include "storage/perfschema/pfs_latch_source.h"

#include <string.h>
#include <algorithm>

#include "my_murmur3.h"
#include "my_sys.h"
#include "storage/perfschema/pfs_builtin_memory.h"
#include "storage/perfschema/pfs_server.h"
#include "storage/perfschema/pfs_timer.h"

bool flag_latch_source_summary = false;
size_t latch_source_max = 0;
ulong latch_source_lost = 0;
ulong latch_source_holds_lost = 0;
ulong latch_source_unmatched_releases = 0;
PFS_latch_source_stat *latch_source_stat_array = nullptr;

namespace {

constexpr uint32 LATCH_SOURCE_SLOT_FREE = 0;
constexpr uint32 LATCH_SOURCE_SLOT_BUSY = 1;
constexpr uint32 LATCH_SOURCE_SLOT_READY = 2;

/** Maximum number of slots visited when looking for a key. */
constexpr size_t LATCH_SOURCE_MAX_PROBES = 64;
/** Waits a thread can have started and not yet ended. */
constexpr uint LATCH_SOURCE_PENDING_SIZE = 8;
/** Locks a thread can hold with hold time measured. */
constexpr uint LATCH_SOURCE_HELD_SIZE = 32;

/** Incremented by TRUNCATE TABLE, invalidates thread local row pointers. */
std::atomic<uint64> latch_source_generation{0};

struct Pending_wait {
  const void *m_state;
  PFS_latch_source_stat *m_row;
  uint64 m_generation;
};

struct Held_latch {
  const void *m_identity;
  PFS_latch_source_stat *m_row;
  /** Start of the current hold interval, 0 while in a condition wait. */
  ulonglong m_start;
  uint64 m_generation;
  PFS_latch_mode m_mode;
};

thread_local Pending_wait t_pending[LATCH_SOURCE_PENDING_SIZE];
thread_local uint t_pending_count = 0;
thread_local Held_latch t_held[LATCH_SOURCE_HELD_SIZE];
thread_local uint t_held_count = 0;

uint32 hash_key(const PFS_latch_source_key &key) {
  const auto klass = reinterpret_cast<uintptr_t>(key.m_class);
  const uint32 seed = static_cast<uint32>(klass ^ (klass >> 32)) ^
                      (key.m_line * 2654435761U) ^ (key.m_operation << 24);
  return murmur3_32(reinterpret_cast<const uchar *>(key.m_file),
                    key.m_file_length, seed);
}

bool key_equal(const PFS_latch_source_key &a, const PFS_latch_source_key &b) {
  return a.m_class == b.m_class && a.m_operation == b.m_operation &&
         a.m_line == b.m_line && a.m_file_length == b.m_file_length &&
         memcmp(a.m_file, b.m_file, a.m_file_length) == 0;
}

PFS_latch_source_stat *find_or_create(PFS_instr_class *klass, uint operation,
                                      const char *src_file, uint src_line) {
  PFS_latch_source_key key;
  key.m_class = klass;
  key.m_operation = operation;
  key.m_line = src_line;
  const char *base = (src_file == nullptr) ? "" : base_name(src_file);
  key.m_file_length = static_cast<uint>(
      std::min<size_t>(strlen(base), PFS_LATCH_SOURCE_FILE_LENGTH));
  memcpy(key.m_file, base, key.m_file_length);

  const uint32 hash = hash_key(key);
  const size_t probes = std::min(latch_source_max, LATCH_SOURCE_MAX_PROBES);
  size_t index = hash % latch_source_max;

  for (size_t probe = 0; probe < probes;
       probe++, index = (index + 1) % latch_source_max) {
    PFS_latch_source_stat *row = &latch_source_stat_array[index];
    uint32 state = row->m_state.load(std::memory_order_acquire);

    if (state == LATCH_SOURCE_SLOT_FREE) {
      if (row->m_state.compare_exchange_strong(state, LATCH_SOURCE_SLOT_BUSY,
                                               std::memory_order_acq_rel)) {
        row->m_hash = hash;
        row->m_key = key;
        row->m_wait.reset();
        row->m_hold.reset();
        row->m_state.store(LATCH_SOURCE_SLOT_READY, std::memory_order_release);
        return row;
      }
      /* Another thread claimed the slot, state now holds its value. */
    }

    /* Another thread is writing the key: wait briefly, it takes a few
    stores. Give up on the slot if it takes too long. */
    for (uint spin = 0; state == LATCH_SOURCE_SLOT_BUSY && spin < 1000;
         spin++) {
      state = row->m_state.load(std::memory_order_acquire);
    }

    if (state == LATCH_SOURCE_SLOT_READY && row->m_hash == hash &&
        key_equal(row->m_key, key)) {
      return row;
    }
  }

  latch_source_lost++;
  return nullptr;
}

ulonglong hold_end(const Held_latch &held) {
  return (held.m_start == 0) ? 0 : get_wait_timer();
}

void aggregate_hold(const Held_latch &held, ulonglong end) {
  if (held.m_start != 0 && end >= held.m_start &&
      held.m_generation ==
          latch_source_generation.load(std::memory_order_relaxed)) {
    held.m_row->m_hold.aggregate_value(end - held.m_start);
  }
}

bool mode_matches(PFS_latch_mode held, PFS_latch_mode released) {
  return released == PFS_latch_mode::ANY || held == PFS_latch_mode::ANY ||
         held == released;
}

/** @return index of the newest held entry for the lock, or -1. */
int find_held(const void *identity, PFS_latch_mode mode) {
  for (int i = static_cast<int>(t_held_count) - 1; i >= 0; i--) {
    if (t_held[i].m_identity == identity &&
        mode_matches(t_held[i].m_mode, mode)) {
      return i;
    }
  }
  return -1;
}

}  // namespace

bool PFS_latch_source_stat::is_populated() const {
  return m_state.load(std::memory_order_acquire) == LATCH_SOURCE_SLOT_READY;
}

int init_latch_source(const PFS_global_param *param) {
  latch_source_lost = 0;
  latch_source_holds_lost = 0;
  latch_source_unmatched_releases = 0;
  latch_source_stat_array = nullptr;
  latch_source_max = (param->m_latch_source_sizing > 0)
                         ? static_cast<size_t>(param->m_latch_source_sizing)
                         : 0;

  if (latch_source_max == 0) {
    return 0;
  }

  latch_source_stat_array = PFS_MALLOC_ARRAY(
      &builtin_memory_latch_source, latch_source_max,
      sizeof(PFS_latch_source_stat), PFS_latch_source_stat, MYF(MY_ZEROFILL));

  if (unlikely(latch_source_stat_array == nullptr)) {
    latch_source_max = 0;
    return 1;
  }

  for (size_t index = 0; index < latch_source_max; index++) {
    latch_source_stat_array[index].m_wait.reset();
    latch_source_stat_array[index].m_hold.reset();
  }

  return 0;
}

void cleanup_latch_source() {
  PFS_FREE_ARRAY(&builtin_memory_latch_source, latch_source_max,
                 sizeof(PFS_latch_source_stat), latch_source_stat_array);
  latch_source_stat_array = nullptr;
  latch_source_max = 0;
}

void reset_latch_source() {
  latch_source_generation.fetch_add(1, std::memory_order_relaxed);

  /* Only free complete rows: a slot being written by another thread stays
  owned by that thread, so two threads never write the same key. */
  for (size_t index = 0; index < latch_source_max; index++) {
    uint32 state = LATCH_SOURCE_SLOT_READY;
    latch_source_stat_array[index].m_state.compare_exchange_strong(
        state, LATCH_SOURCE_SLOT_FREE, std::memory_order_acq_rel);
  }

  latch_source_lost = 0;
  latch_source_holds_lost = 0;
  latch_source_unmatched_releases = 0;
}

void latch_source_start_wait(const void *state, PFS_instr_class *klass,
                             uint operation, const char *src_file,
                             uint src_line) {
  if (latch_source_stat_array == nullptr) {
    return;
  }

  PFS_latch_source_stat *row =
      find_or_create(klass, operation, src_file, src_line);
  if (row == nullptr) {
    return;
  }

  if (t_pending_count == LATCH_SOURCE_PENDING_SIZE) {
    /* Waits that never ended (consumer disabled meanwhile): drop oldest. */
    memmove(&t_pending[0], &t_pending[1],
            (LATCH_SOURCE_PENDING_SIZE - 1) * sizeof(Pending_wait));
    t_pending_count--;
  }

  t_pending[t_pending_count++] = {
      state, row, latch_source_generation.load(std::memory_order_relaxed)};
}

void latch_source_end_wait(const void *state, const void *identity,
                           PFS_latch_mode mode, int rc, ulonglong timer_start,
                           ulonglong timer_end) {
  int found = -1;
  for (int i = static_cast<int>(t_pending_count) - 1; i >= 0; i--) {
    if (t_pending[i].m_state == state) {
      found = i;
      break;
    }
  }

  if (found < 0) {
    return;
  }

  const Pending_wait pending = t_pending[found];
  /* Entries above it belong to waits that never ended, drop them too. */
  t_pending_count = static_cast<uint>(found);

  if (pending.m_generation !=
      latch_source_generation.load(std::memory_order_relaxed)) {
    return;
  }

  pending.m_row->m_wait.aggregate_value(timer_end - timer_start);

  if (rc != 0) {
    return;
  }

  if (t_held_count == LATCH_SOURCE_HELD_SIZE) {
    /* Oldest entry is most likely a lock released by another thread. */
    memmove(&t_held[0], &t_held[1],
            (LATCH_SOURCE_HELD_SIZE - 1) * sizeof(Held_latch));
    t_held_count--;
    latch_source_holds_lost++;
  }

  t_held[t_held_count++] = {identity, pending.m_row, timer_end,
                            pending.m_generation, mode};
}

void latch_source_unlock(const void *identity, PFS_latch_mode mode,
                         bool count_unmatched) {
  if (latch_source_stat_array == nullptr) {
    return;
  }

  const int found = find_held(identity, mode);

  if (found < 0) {
    if (count_unmatched) {
      latch_source_unmatched_releases++;
    }
    return;
  }

  const Held_latch held = t_held[found];
  memmove(&t_held[found], &t_held[found + 1],
          (t_held_count - found - 1) * sizeof(Held_latch));
  t_held_count--;

  aggregate_hold(held, hold_end(held));
}

void latch_source_cond_wait_start(const void *mutex_identity) {
  const int found = find_held(mutex_identity, PFS_latch_mode::ANY);
  if (found < 0) {
    return;
  }

  Held_latch &held = t_held[found];
  aggregate_hold(held, hold_end(held));
  held.m_start = 0;
}

void latch_source_cond_wait_end(const void *mutex_identity) {
  for (int i = static_cast<int>(t_held_count) - 1; i >= 0; i--) {
    if (t_held[i].m_identity == mutex_identity && t_held[i].m_start == 0) {
      t_held[i].m_start = get_wait_timer();
      return;
    }
  }
}
