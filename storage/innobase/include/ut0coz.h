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

Latches acquired with a non-zero pass value (rw-lock "pass", e.g. BUF_IO_READ
and BUF_IO_WRITE for block latches handed over to asynchronous I/O) are
reported as hand-off latches: they are usually released by another thread (the
I/O handler thread when the I/O completes), so they must not make the
acquiring thread a lock holder for Coz, and their release must not count as a
release by the releasing thread. The release still lets the releasing thread
catch up on Coz delays before waiters wake up. The pass value decides, not the
call site, so every such latch is covered.

The pre_block()/post_block() hooks bracket waits that Coz cannot see, e.g.
polling until another thread completes an I/O. Waits for the kernel or the
storage device (io_getevents(), synchronous pread()/pwrite()) are not
bracketed: the device runs outside Coz virtual time, like the benchmark client,
so a thread woken by the device must still pay the delays inserted meanwhile.

The ABI matches the "coz-mcp latch hooks" sections of coz.h in the Coz fork:
libcoz exports _coz_get_latch_api2() returning a table of seven functions; an
older libcoz has only _coz_get_latch_api() with the first three, then hand-off
latches are reported like other latches. */

#ifndef ut0coz_h
#define ut0coz_h

/* Not in UNIV_LIBRARY builds (e.g. innodb_zipdecompress for offline tools):
they have no latch meta data and never run under libcoz. */
#if defined(UNIV_COZ_HOOKS) && !defined(UNIV_LIBRARY)

#include <dlfcn.h>

#include "sync0types.h"

namespace ut_coz {

/** Latch modes, as in coz.h (COZ_LATCH_MUTEX, ...). */
enum mode_t : int { MUTEX = 0, S = 1, X = 2, SX = 3 };

/** Function table exported by libcoz (coz_latch_api2_t in coz.h; the first
three members are coz_latch_api_t). */
struct api_t {
  void (*wait_begin)(const void *latch, const char *file, int line,
                     const char *name, int mode);
  void (*acquired)(const void *latch);
  void (*release)(const void *latch);
  void (*acquired_handoff)(const void *latch);
  void (*release_handoff)(const void *latch);
  void (*pre_block)();
  void (*post_block)(int skip_delays);
};

/** @return the table filled in from an older libcoz with only the three
function table, or nullptr when libcoz is not loaded */
inline api_t *api_v1() {
  using get_api_t = api_t *(*)();
  auto get =
      reinterpret_cast<get_api_t>(dlsym(RTLD_DEFAULT, "_coz_get_latch_api"));
  if (get == nullptr) return nullptr;
  api_t *v1 = get();
  static api_t table;
  table.wait_begin = v1->wait_begin;
  table.acquired = v1->acquired;
  table.release = v1->release;
  table.acquired_handoff = v1->acquired;
  table.release_handoff = v1->release;
  table.pre_block =
      reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "_coz_pre_block"));
  table.post_block =
      reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT, "_coz_post_block"));
  if (table.pre_block == nullptr || table.post_block == nullptr) {
    return nullptr;
  }
  return &table;
}

/** @return libcoz's latch hook table, or nullptr when libcoz is not loaded */
inline api_t *api() {
  static api_t *const table = [] {
    using get_api_t = api_t *(*)();
    auto get =
        reinterpret_cast<get_api_t>(dlsym(RTLD_DEFAULT, "_coz_get_latch_api2"));
    return get != nullptr ? get() : api_v1();
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

/** Report that the thread acquired a latch.
@param[in]  latch  the latch
@param[in]  pass   rw-lock pass value: non-zero for a hand-off latch */
inline void acquired(const void *latch, ulint pass = 0) {
  if (api_t *a = api()) {
    if (pass == 0) {
      a->acquired(latch);
    } else {
      a->acquired_handoff(latch);
    }
  }
}

/** Report that a latch is about to be released.
@param[in]  latch  the latch
@param[in]  pass   pass value it was acquired with: non-zero for a hand-off
                   latch, which may be released by any thread */
inline void release(const void *latch, ulint pass = 0) {
  if (api_t *a = api()) {
    if (pass == 0) {
      a->release(latch);
    } else {
      a->release_handoff(latch);
    }
  }
}

/** Report that the thread starts a wait that Coz cannot see (e.g. polling). */
inline void pre_block() {
  if (api_t *a = api()) a->pre_block();
}

/** Report the end of a wait started with pre_block().
@param[in]  by_thread  true when another thread ended the wait (Coz skips the
                       delays inserted meanwhile), false on timeout or when
                       the wait ended for another reason */
inline void post_block(bool by_thread) {
  if (api_t *a = api()) a->post_block(by_thread ? 1 : 0);
}

/** Reports wait_begin on construction and acquired on destruction, for
blocking acquire functions with several return paths. */
class Wait_guard {
 public:
  Wait_guard(const void *latch, const char *file, ulint line, latch_id_t id,
             mode_t mode, ulint pass)
      : m_latch(latch), m_pass(pass) {
    wait_begin(latch, file, line, id, mode);
  }
  ~Wait_guard() { acquired(m_latch, m_pass); }
  Wait_guard(const Wait_guard &) = delete;
  Wait_guard &operator=(const Wait_guard &) = delete;

 private:
  const void *m_latch;
  ulint m_pass;
};

/** For try-lock functions: when ok, reports a zero-length wait at the site and
the acquisition (libcoz records an acquisition only after wait_begin).
@return ok */
inline bool acquired_if(bool ok, const void *latch, const char *file,
                        ulint line, latch_id_t id, mode_t mode,
                        ulint pass = 0) {
  if (ok) {
    wait_begin(latch, file, line, id, mode);
    acquired(latch, pass);
  }
  return ok;
}

}  // namespace ut_coz

/** Report a blocking acquisition: wait begins here, acquired at scope exit.
A non-zero pass value makes it a hand-off latch. */
#define UT_COZ_WAIT_GUARD(latch, file, line, id, mode, pass)                  \
  ut_coz::Wait_guard ut_coz_wait_guard((latch), (file), (line), (id), (mode), \
                                       (pass))
#define UT_COZ_WAIT_BEGIN(latch, file, line, id, mode) \
  ut_coz::wait_begin((latch), (file), (line), (id), (mode))
#define UT_COZ_ACQUIRED(latch) ut_coz::acquired(latch)
#define UT_COZ_ACQUIRED_IF(ok, latch, file, line, id, mode, pass) \
  ut_coz::acquired_if((ok), (latch), (file), (line), (id), (mode), (pass))
#define UT_COZ_TRY_ACQUIRED(latch, file, line, id, mode) \
  (void)ut_coz::acquired_if(true, (latch), (file), (line), (id), (mode))
#define UT_COZ_RELEASE(latch) ut_coz::release(latch)
/** Report the release of an rw-lock acquired with the given pass value. */
#define UT_COZ_RELEASE_PASS(latch, pass) ut_coz::release((latch), (pass))
#define UT_COZ_PRE_BLOCK() ut_coz::pre_block()
#define UT_COZ_POST_BLOCK(by_thread) ut_coz::post_block(by_thread)

#else /* UNIV_COZ_HOOKS && !UNIV_LIBRARY */

#define UT_COZ_WAIT_GUARD(latch, file, line, id, mode, pass)
#define UT_COZ_WAIT_BEGIN(latch, file, line, id, mode)
#define UT_COZ_ACQUIRED(latch)
#define UT_COZ_ACQUIRED_IF(ok, latch, file, line, id, mode, pass) (ok)
#define UT_COZ_TRY_ACQUIRED(latch, file, line, id, mode)
#define UT_COZ_RELEASE(latch)
#define UT_COZ_RELEASE_PASS(latch, pass)
#define UT_COZ_PRE_BLOCK()
#define UT_COZ_POST_BLOCK(by_thread)

#endif /* UNIV_COZ_HOOKS && !UNIV_LIBRARY */

#endif /* ut0coz_h */
