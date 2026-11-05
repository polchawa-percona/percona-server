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

/** @file sync/sync0lru_hold.cc
 CSV dump of the per-call-site LRU list mutex statistics collected in
 lru_hold_stats (see sync0lru_hold.h). */

#include "sync0lru_hold.h"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "fil0fil.h"
#include "my_io.h"
#include "os0event.h"
#include "srv0shutdown.h"
#include "srv0srv.h"
#include "ut0log.h"

os_event_t srv_lru_hold_stats_event;

namespace {

/** How often a snapshot of the registry is appended to the CSV file. */
constexpr std::chrono::milliseconds DUMP_INTERVAL{1000};

/** Name of the CSV file, created in the data directory. */
constexpr const char *CSV_NAME = "lru_mutex_contention_stats.csv";

/** @return true if InnoDB has started shutting down. */
bool shutting_down() {
  return srv_shutdown_state.load(std::memory_order_relaxed) >=
         SRV_SHUTDOWN_CLEANUP;
}

/** Build the path of the CSV file, in the same directory buf0dump uses.
@param[out] path       buffer for the path
@param[in]  path_size  size of the buffer */
void generate_csv_path(char *path, size_t path_size) {
  const char *dir = strcmp(srv_data_home, "") == 0
                        ? static_cast<const char *>(MySQL_datadir_path)
                        : srv_data_home;
  snprintf(path, path_size, "%s%c%s", dir, OS_PATH_SEPARATOR, CSV_NAME);
}

/** Append one CSV row per call-site that has at least one sample.

The file is opened only when there is something to write, and closed again
after each snapshot, so it can be removed or moved away while the server
runs.
@param[in]  path  path of the CSV file */
void dump_snapshot(const char *path) {
  const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  FILE *f = nullptr;
  bool failed = false;

  lru_hold_stats.iterate([&](const char *file, unsigned line, uint64_t count,
                             uint64_t total_ns, uint64_t max_ns,
                             uint64_t wait_total_ns, uint64_t wait_max_ns) {
    if (f == nullptr) {
      if (failed) {
        return;
      }
      f = fopen(path, "a");
      if (f == nullptr) {
        failed = true;
        ib::error() << "lru_hold_stats: failed to open '" << path
                    << "' for writing, errno=" << errno;
        return;
      }
      if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == 0) {
        /* Same columns as the block mutex contention CSV, without the
        wait-time histogram. */
        fprintf(f,
                "ts_us,site,count,total_wait_ns,max_wait_ns,total_held_ns,"
                "max_held_ns\n");
      }
    }

    fprintf(f,
            "%lld,%s:%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 "\n",
            static_cast<long long>(now_us), file, line, count, wait_total_ns,
            wait_max_ns, total_ns, max_ns);
  });

  if (f != nullptr) {
    fclose(f);
  }
}

}  // namespace

void lru_hold_stats_thread() {
  char path[FN_REFLEN];
  generate_csv_path(path, sizeof(path));

  while (!shutting_down()) {
    os_event_wait_time(srv_lru_hold_stats_event, DUMP_INTERVAL);
    os_event_reset(srv_lru_hold_stats_event);
    dump_snapshot(path);
  }

  /* Final snapshot so the last interval before shutdown is not lost. */
  dump_snapshot(path);
}
