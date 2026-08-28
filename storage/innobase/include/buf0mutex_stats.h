/** @file include/buf0mutex_stats.h
 Per-call-site instrumentation of buf_block_t/buf_page_t mutex acquisitions
 (PS-11120: InnoDB block mutex contention on hot-page workloads).

 Every place that acquires a page/block mutex is auto-registered, by its
 file:line, the first time it runs. Each registered site keeps an atomic
 call count, total wait time and max wait time (in CPU cycles, converted to
 nanoseconds when dumped), plus a small log2 histogram of wait times, and
 total/max hold time (time actually spent inside the critical section,
 between a successful acquire and the matching release). A background
 thread (buf_mutex_stats_thread) periodically appends a snapshot of all
 sites to a CSV file.

 Hold time is tracked via a small thread-local stack: BUF_MUTEX_ENTER_
 INSTRUMENTED pushes {mutex address, site, acquire time} after acquiring,
 and BUF_MUTEX_EXIT_INSTRUMENTED looks the address up (by value, so it is
 immune to variable aliasing) and pops it before releasing. A handful of
 call sites hand the mutex off to be released deep inside shared code
 instead of releasing it themselves (e.g. buf_LRU_free_page() releasing it,
 on success, as part of freeing the page); the actual release in that
 shared code is instrumented too (see buf_LRU_block_remove_hashed() and
 buf_flush_page() in the .cc files), so the entry still gets matched and
 correctly attributed to the site that originally pushed it. The only
 entries that go permanently unmatched are ones dropped for stack overflow
 (MAX_NESTING) or released via some path genuinely outside this scheme -
 both harmless, silently-ignored cases (see buf_mutex_stats_on_exit()).

 This is a diagnostic-only facility, gated by UNIV_BUF_MUTEX_STATS so it can
 be compiled out entirely (e.g. for UNIV_LIBRARY/UNIV_HOTBACKUP builds, or a
 clean baseline build for comparison). */

#ifndef buf0mutex_stats_h
#define buf0mutex_stats_h

#include "univ.i"

#if !defined(UNIV_HOTBACKUP) && !defined(UNIV_LIBRARY)
/** Compiled in by default on this branch; undefine to get an uninstrumented
baseline build for comparison. */
#define UNIV_BUF_MUTEX_STATS 1
#endif /* !UNIV_HOTBACKUP && !UNIV_LIBRARY */

#ifdef UNIV_BUF_MUTEX_STATS

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "my_macros.h"
#include "my_rdtsc.h"
#include "os0event.h"
#include "ut0cpu_cache.h"

/** One registered mutex-acquisition call site. */
struct alignas(ut::INNODB_CACHE_LINE_SIZE) Buf_mutex_site_stats {
  /** Number of times mutex_enter() completed at this site. */
  std::atomic<uint64_t> count{0};

  /** Sum of wait times (TSC cycles) spent inside mutex_enter() at this
  site. */
  std::atomic<uint64_t> total_cycles{0};

  /** Longest single wait (TSC cycles) observed at this site. Updated with
  a relaxed compare-exchange loop; a lost race under concurrency just means
  a slightly stale max, which is acceptable for this diagnostic. */
  std::atomic<uint64_t> max_cycles{0};

  /** log2-bucketed histogram of wait times: bucket i counts waits with
  cycles in [2^i, 2^(i+1)). Bucket 0 catches cycles == 0. */
  static constexpr size_t N_BUCKETS = 48;
  std::atomic<uint64_t> buckets[N_BUCKETS] = {};

  /** Sum of hold times (TSC cycles) spent between acquiring and releasing
  the mutex at this site. Only counts acquisitions whose release was also
  routed through BUF_MUTEX_EXIT_INSTRUMENTED (see file header). */
  std::atomic<uint64_t> held_cycles{0};

  /** Longest single hold time (TSC cycles) observed at this site. Same
  relaxed-CAS-loop tolerance as max_cycles above. */
  std::atomic<uint64_t> held_max_cycles{0};

  /** "file:line" label; nullptr until the site is registered. Published
  with release semantics by register_site(); read with acquire before
  use, so a reader either sees the fully-registered site or skips it. */
  std::atomic<const char *> label{nullptr};
};

/** Registry of all instrumented page/block mutex acquisition sites. */
class Buf_mutex_stats_registry {
 public:
  /** Hard cap on distinct call sites. There are only a few dozen actual
  call sites of the page/block mutex in the source; this leaves generous
  headroom. */
  static constexpr size_t MAX_SITES = 128;

  /** Registers a new call site and reserves its stats slot. Called once
  per distinct call site, from a function-local static initializer, so
  concurrent callers always reserve distinct indexes.
  @param[in]  label  "file:line" of the call site; must be a
                      compile-time string literal (stored by pointer).
  @return index of the site's stats slot */
  size_t register_site(const char *label);

  /** @return the stats slot for site index `i`. */
  Buf_mutex_site_stats &site(size_t i) { return m_sites[i]; }

 private:
  Buf_mutex_site_stats m_sites[MAX_SITES];

  /** Next free slot to hand out. Sites are looked up by scanning
  m_sites[] for a non-null label (see site()/label), not by this
  counter, so its value never needs to be published to readers. */
  std::atomic<size_t> m_next_idx{0};
};

/** The single global registry instance. */
extern Buf_mutex_stats_registry buf_mutex_stats;

/** Records one completed mutex acquisition at the given site.
@param[in]  site_idx  index returned by Buf_mutex_stats_registry::register_site
@param[in]  cycles    TSC cycles spent inside mutex_enter() */
void buf_mutex_stats_record(size_t site_idx, uint64_t cycles);

/** Pushes a "this thread now holds this mutex, acquired at this site and
time" entry onto a small thread-local stack, for hold-time tracking.
@param[in]  site_idx   index returned by register_site()
@param[in]  mutex_ptr  address of the mutex just acquired (identifies the
                       entry to buf_mutex_stats_on_exit(), regardless of
                       which variable/alias is used to release it) */
void buf_mutex_stats_on_enter(size_t site_idx, const void *mutex_ptr);

/** Looks up the thread-local entry pushed by buf_mutex_stats_on_enter() for
this exact mutex address, and if found, records the elapsed hold time
against that entry's site and removes it from the stack. A miss (e.g. the
matching enter's push was dropped because the thread-local stack was full,
or this mutex was released via a path other than BUF_MUTEX_EXIT_INSTRUMENTED)
is silently ignored - this is a best-effort diagnostic, not a correctness
mechanism.
@param[in]  mutex_ptr  address of the mutex about to be released */
void buf_mutex_stats_on_exit(const void *mutex_ptr);

/** Acquires the given mutex, timing the wait and recording it under a
call site that is auto-registered (by file:line) the first time this
expands at a given point in the source. Also pushes a hold-time-tracking
entry (see buf_mutex_stats_on_enter()).
@param[in]  mutex_ptr  pointer to the block/page mutex to enter */
#define BUF_MUTEX_ENTER_INSTRUMENTED(mutex_ptr)                              \
  do {                                                                       \
    static const size_t buf_mutex_stats_site_ =                              \
        buf_mutex_stats.register_site(__FILE__ ":" STRINGIFY_ARG(__LINE__)); \
    const uint64_t buf_mutex_stats_t0_ = my_timer_cycles();                  \
    mutex_enter(mutex_ptr);                                                  \
    buf_mutex_stats_record(buf_mutex_stats_site_,                            \
                           my_timer_cycles() - buf_mutex_stats_t0_);         \
    buf_mutex_stats_on_enter(buf_mutex_stats_site_,                          \
                             static_cast<const void *>(mutex_ptr));          \
  } while (0)

/** Releases the given mutex, first recording the hold time since the
matching BUF_MUTEX_ENTER_INSTRUMENTED (see buf_mutex_stats_on_exit()).
@param[in]  mutex_ptr  pointer to the block/page mutex to exit */
#define BUF_MUTEX_EXIT_INSTRUMENTED(mutex_ptr)                     \
  do {                                                             \
    buf_mutex_stats_on_exit(static_cast<const void *>(mutex_ptr)); \
    mutex_exit(mutex_ptr);                                         \
  } while (0)

/** Event used to wake up (or force an early tick of) the CSV-dumping
background thread; also signaled at shutdown so it can exit promptly. */
extern os_event_t srv_buf_mutex_stats_event;

/** Background thread that periodically snapshots the registry to a CSV
file (buf_mutex_contention_stats.csv, in the datadir). Started next to the
buffer pool dump thread, stopped the same way at shutdown. */
void buf_mutex_stats_thread();

#else /* UNIV_BUF_MUTEX_STATS */

/** Uninstrumented fallback: plain mutex_enter()/mutex_exit(), for builds
where UNIV_BUF_MUTEX_STATS is off (UNIV_LIBRARY/UNIV_HOTBACKUP, or a
baseline build for comparison). Callers use these macros unconditionally
either way.
@param[in]  mutex_ptr  pointer to the block/page mutex to enter/exit */
#define BUF_MUTEX_ENTER_INSTRUMENTED(mutex_ptr) \
  do {                                          \
    mutex_enter(mutex_ptr);                     \
  } while (0)

#define BUF_MUTEX_EXIT_INSTRUMENTED(mutex_ptr) \
  do {                                         \
    mutex_exit(mutex_ptr);                     \
  } while (0)

#endif /* UNIV_BUF_MUTEX_STATS */

#endif /* buf0mutex_stats_h */
