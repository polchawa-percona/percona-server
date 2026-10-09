/*****************************************************************************

Copyright (c) 2026, Percona Inc. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/ut0coz.h
Percona: latch hooks for coz-mcp (https://github.com/polchawa-percona/coz-mcp).

Built only with the CMake option WITH_COZ (defines UNIV_COZ_HOOKS). When the
server runs under the coz-mcp version of libcoz (LD_PRELOAD), every InnoDB
mutex and rw-lock acquisition and release is reported to it, so that:
- slowdown experiments on lines inside a critical section are paid while the
  latch is held and virtual speedup delays are not paid while it is held;
- latch wait and hold times can be ranked per acquire site.
Without libcoz loaded each hook costs one predictable branch.

The ABI matches the "coz-mcp latch hooks" section of coz.h in the Coz fork:
libcoz exports _coz_get_latch_api() returning a table of three functions. */

#ifndef ut0coz_h
#define ut0coz_h

#ifdef UNIV_COZ_HOOKS

#include <dlfcn.h>

#include "sync0types.h"

namespace ut_coz {

/** Latch modes, as in coz.h (COZ_LATCH_MUTEX, ...). */
enum mode_t : int { MUTEX = 0, S = 1, X = 2, SX = 3 };

/** Function table exported by libcoz (coz_latch_api_t in coz.h). */
struct api_t {
  void (*wait_begin)(const void *latch, const char *file, int line,
                     const char *name, int mode);
  void (*acquired)(const void *latch);
  void (*release)(const void *latch);
};

/** @return libcoz's latch hook table, or nullptr when libcoz is not loaded */
inline api_t *api() {
  static api_t *const table = [] {
    using get_api_t = api_t *(*)();
    auto get =
        reinterpret_cast<get_api_t>(dlsym(RTLD_DEFAULT, "_coz_get_latch_api"));
    return get != nullptr ? get() : nullptr;
  }();
  return table;
}

/** @return latch name for statistics; latch meta data may already be gone
during shutdown */
inline const char *latch_name(latch_id_t id) {
  if (id == LATCH_ID_NONE || static_cast<size_t>(id) >= latch_meta.size() ||
      latch_meta[id] == nullptr) {
    return "unknown";
  }
  return latch_meta[id]->get_name();
}

/** Report that the thread starts waiting for (acquiring) a latch. */
inline void wait_begin(const void *latch, const char *file, ulint line,
                       latch_id_t id, mode_t mode) {
  if (api_t *a = api()) {
    a->wait_begin(latch, file, static_cast<int>(line), latch_name(id), mode);
  }
}

/** Report that the thread acquired a latch. */
inline void acquired(const void *latch) {
  if (api_t *a = api()) a->acquired(latch);
}

/** Report that the thread is about to release a latch. */
inline void release(const void *latch) {
  if (api_t *a = api()) a->release(latch);
}

/** Reports wait_begin on construction and acquired on destruction, for
blocking acquire functions with several return paths. */
class Wait_guard {
 public:
  Wait_guard(const void *latch, const char *file, ulint line, latch_id_t id,
             mode_t mode)
      : m_latch(latch) {
    wait_begin(latch, file, line, id, mode);
  }
  ~Wait_guard() { acquired(m_latch); }
  Wait_guard(const Wait_guard &) = delete;
  Wait_guard &operator=(const Wait_guard &) = delete;

 private:
  const void *m_latch;
};

/** For try-lock functions: when ok, reports a zero-length wait at the site and
the acquisition (libcoz records an acquisition only after wait_begin).
@return ok */
inline bool acquired_if(bool ok, const void *latch, const char *file,
                        ulint line, latch_id_t id, mode_t mode) {
  if (ok) {
    wait_begin(latch, file, line, id, mode);
    acquired(latch);
  }
  return ok;
}

}  // namespace ut_coz

/** Report a blocking acquisition: wait begins here, acquired at scope exit. */
#define UT_COZ_WAIT_GUARD(latch, file, line, id, mode) \
  ut_coz::Wait_guard ut_coz_wait_guard((latch), (file), (line), (id), (mode))
#define UT_COZ_WAIT_BEGIN(latch, file, line, id, mode) \
  ut_coz::wait_begin((latch), (file), (line), (id), (mode))
#define UT_COZ_ACQUIRED(latch) ut_coz::acquired(latch)
#define UT_COZ_ACQUIRED_IF(ok, latch, file, line, id, mode) \
  ut_coz::acquired_if((ok), (latch), (file), (line), (id), (mode))
#define UT_COZ_TRY_ACQUIRED(latch, file, line, id, mode) \
  (void)ut_coz::acquired_if(true, (latch), (file), (line), (id), (mode))
#define UT_COZ_RELEASE(latch) ut_coz::release(latch)

#else /* UNIV_COZ_HOOKS */

#define UT_COZ_WAIT_GUARD(latch, file, line, id, mode)
#define UT_COZ_WAIT_BEGIN(latch, file, line, id, mode)
#define UT_COZ_ACQUIRED(latch)
#define UT_COZ_ACQUIRED_IF(ok, latch, file, line, id, mode) (ok)
#define UT_COZ_TRY_ACQUIRED(latch, file, line, id, mode)
#define UT_COZ_RELEASE(latch)

#endif /* UNIV_COZ_HOOKS */

#endif /* ut0coz_h */
