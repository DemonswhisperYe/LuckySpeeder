/*

MIT License

Copyright (c) 2024 kekeimiku
Copyright (c) 2024 ac0d3r

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "LuckySpeeder.h"
#include "fishhook.h"
#include "clock_core.h"
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if !TARGET_OS_TV
#include "hwbphook.h"
#endif

// ---- real clock providers ----
// The three hooked entries (clock_gettime/gettimeofday/mach_absolute_time)
// carry hardware breakpoints on their FIRST instruction. Calling any of
// them from inside a hook re-triggers the breakpoint and recurses into
// the hook (recursive os_unfair_lock -> abort; crash log 1806). Real
// time therefore must NEVER come from the hooked symbols:
//   - monotonic: mach_continuous_time (separate symbol, same timebase,
//     not breakpointed, reads the commpage so it is cheap)
//   - wall: anchored once via raw syscall (immune to userland hooks),
//     then derived as anchor + continuous ticks
static int (*sys_clock_gettime)(clockid_t, struct timespec *);
static int (*sys_gettimeofday)(struct timeval *, void *);
static uint64_t (*sys_mach_absolute_time)(void);
static uint64_t (*sys_mach_continuous_time)(void);
static uint64_t (*sys_clock_gettime_nsec_np)(clockid_t);
static double (*sys_CACurrentMediaTime)(void);
static double (*sys_CFAbsoluteTimeGetCurrent)(void);
static time_t (*sys_time)(time_t *);
static mach_timebase_info_data_t tb_cache;
static int64_t rt_anchor_ns; // true wall clock at anchor minus continuous ns

static uint64_t real_ticks(void) {
  return sys_mach_continuous_time();
}

static int64_t real_rt_ns(void) {
  return rt_anchor_ns + (int64_t)clock_core_ns_from_ticks(real_ticks());
}

static int bind_real_clocks(void) {
  sys_mach_continuous_time = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "mach_continuous_time");
  if (!sys_mach_continuous_time) return -1;

  mach_timebase_info(&tb_cache);
  clock_core_bind(real_ticks, real_rt_ns, tb_cache.numer, tb_cache.denom);

  int64_t true_rt;
#ifdef SYS_clock_gettime
  struct timespec ts;
  if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts) != 0) return -1;
  true_rt = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#else
  // tvOS/macCatalyst: syscall(2) unavailable; read the wall clock before
  // any breakpoint exists, so calling gettimeofday directly here is safe
  struct timeval tv;
  gettimeofday(&tv, NULL);
  true_rt = (int64_t)tv.tv_sec * 1000000000LL + (int64_t)tv.tv_usec * 1000;
#endif
  rt_anchor_ns = true_rt - (int64_t)clock_core_ns_from_ticks(sys_mach_continuous_time());
  return 0;
}

// ---- hooked entries: every source reads the same virtual timeline ----

// clock_gettime: per-clk_id handling. All monotonic-family ids share the
// mono line; REALTIME shares the rt line. No shared accumulator across
// kinds (the old code mixed 1.75e9-s wall time and 1e5-s uptime in one
// accumulator and pushed wall time centuries ahead at speed>1).
static int my_clock_gettime(clockid_t clk_id, struct timespec *tp) {
  if (clk_id == CLOCK_REALTIME) {
    int64_t ns = clock_core_realtime_ns();
    tp->tv_sec = (time_t)(ns / 1000000000LL);
    tp->tv_nsec = (long)(ns % 1000000000LL);
    return 0;
  }
  // every other id maps to the mono line, including CLOCK_BOOTTIME(6)
  // which port_clock_gettime used to reject with EINVAL
  uint64_t ns = clock_core_ns_from_ticks(clock_core_mach_abs_time());
  tp->tv_sec = (time_t)(ns / 1000000000LL);
  tp->tv_nsec = (long)(ns % 1000000000LL);
  return 0;
}

static int my_gettimeofday(struct timeval *tv, void *tz) {
  if (!tv) return 0;
  int64_t ns = clock_core_realtime_ns();
  tv->tv_sec = (time_t)(ns / 1000000000LL);
  tv->tv_usec = (suseconds_t)((ns % 1000000000LL) / 1000);
  return 0;
}

static uint64_t my_mach_absolute_time(void) {
  return clock_core_mach_abs_time();
}

// clock_gettime_nsec_np: same timeline as clock_gettime, in ns
static uint64_t my_clock_gettime_nsec_np(clockid_t clk_id) {
  if (clk_id == CLOCK_REALTIME) return (uint64_t)clock_core_realtime_ns();
  return clock_core_ns_from_ticks(clock_core_mach_abs_time());
}

// CACurrentMediaTime: QuartzCore seconds, monotonic line
static double my_CACurrentMediaTime(void) {
  return (double)clock_core_ns_from_ticks(clock_core_mach_abs_time()) / 1000000000.0;
}

// CFAbsoluteTimeGetCurrent: seconds since 2001-01-01, realtime line
static double my_CFAbsoluteTimeGetCurrent(void) {
  const double kCFAbsoluteTimeIntervalSince1970 = 978307200.0;
  return (double)clock_core_realtime_ns() / 1000000000.0 - kCFAbsoluteTimeIntervalSince1970;
}

// time(): epoch seconds, realtime line
static time_t my_time(time_t *tloc) {
  time_t v = (time_t)(clock_core_realtime_ns() / 1000000000LL);
  if (tloc) *tloc = v;
  return v;
}

// ---- hook installation (Club mode) ----

static int hooks_installed = 0;

int hook_clock_gettime(void) {
  if (hooks_installed) return 0;

  sys_clock_gettime = dlsym(RTLD_DEFAULT, "clock_gettime");
  sys_gettimeofday = dlsym(RTLD_DEFAULT, "gettimeofday");
  sys_mach_absolute_time = dlsym(RTLD_DEFAULT, "mach_absolute_time");
  if (!sys_clock_gettime || !sys_gettimeofday || !sys_mach_absolute_time)
    return -1;
  if (bind_real_clocks() != 0) return -1;

  // Two mechanisms, chosen per call frequency:
  //
  // 1) mach_absolute_time is the hottest call in the process (UIKit, GCD
  //    and objc hit it thousands of times per frame). A hardware
  //    breakpoint there costs a Mach exception round-trip per call and
  //    starved the main thread (watchdog 0x8BADF00D, log 191544), so its
  //    GOT slots are rebound instead: zero cost per call.
  //
  // 2) clock_gettime / gettimeofday are low frequency, but the engine's
  //    frame clock reaches them through system-library internal calls that
  //    GOT rebinding cannot see (calls inside libsystem/QuartzCore never
  //    pass through the app's GOT). That is exactly why the fishhook-only
  //    build did not move the game while the breakpoint build did. A
  //    breakpoint sits on the function entry and catches every caller.
  // Every time source the game imports must share one timeline, otherwise
  // the anti-cheat divides an accelerated clock by a real one and reports
  // "abnormal game behaviour". All of these are rebindable through the
  // app's own GOT, so they cost nothing per call.
  sys_clock_gettime_nsec_np = dlsym(RTLD_DEFAULT, "clock_gettime_nsec_np");
  sys_CACurrentMediaTime = dlsym(RTLD_DEFAULT, "CACurrentMediaTime");
  sys_CFAbsoluteTimeGetCurrent = dlsym(RTLD_DEFAULT, "CFAbsoluteTimeGetCurrent");
  sys_time = dlsym(RTLD_DEFAULT, "time");

  struct rebinding rebindings[5] = {
    { "mach_absolute_time", my_mach_absolute_time, (void **)&sys_mach_absolute_time },
    { "clock_gettime_nsec_np", my_clock_gettime_nsec_np, (void **)&sys_clock_gettime_nsec_np },
    { "CACurrentMediaTime", my_CACurrentMediaTime, (void **)&sys_CACurrentMediaTime },
    { "CFAbsoluteTimeGetCurrent", my_CFAbsoluteTimeGetCurrent, (void **)&sys_CFAbsoluteTimeGetCurrent },
    { "time", my_time, (void **)&sys_time },
  };
  if (rebind_symbols(rebindings, 5) != 0) return -1;

#if !TARGET_OS_TV
  void *old[2] = { (void *)sys_clock_gettime, (void *)sys_gettimeofday };
  void *new[2] = { (void *)my_clock_gettime, (void *)my_gettimeofday };
  if (!hwbp_hook(old, new, 2)) return -1;
#endif

  hooks_installed = 1;
  return 0;
}

void set_clock_gettime(float value) {
  clock_core_set_speed((double)value);
}

void reset_clock_gettime(void) {
  if (!hooks_installed) return;

#if !TARGET_OS_TV
  void *old[2] = { (void *)sys_clock_gettime, (void *)sys_gettimeofday };
  hwbp_unhook(old, 2);
#endif

  struct rebinding rebindings[5] = {
    { "mach_absolute_time", (void *)sys_mach_absolute_time, NULL },
    { "clock_gettime_nsec_np", (void *)sys_clock_gettime_nsec_np, NULL },
    { "CACurrentMediaTime", (void *)sys_CACurrentMediaTime, NULL },
    { "CFAbsoluteTimeGetCurrent", (void *)sys_CFAbsoluteTimeGetCurrent, NULL },
    { "time", (void *)sys_time, NULL },
  };
  rebind_symbols(rebindings, 5);
  clock_core_reset();
  hooks_installed = 0;
}

// Diamond mode (mach_absolute_time via fishhook) is unchanged; it exists
// as a fallback mode and games whose only clock is mach_absolute_time.
static float mach_absolute_time_speed = 1.0;

static uint64_t mach_absolute_base_time = 0;
static uint64_t mach_absolute_start_time = 0;
static uint64_t mach_absolute_last_time = 0;
static os_unfair_lock mach_absolute_base_time_lock = OS_UNFAIR_LOCK_INIT;

static uint64_t (*original_mach_absolute_time)(void) = NULL;

static uint64_t my_mach_absolute_time_diamond(void) {
  os_unfair_lock_lock(&mach_absolute_base_time_lock);
  uint64_t current_time = original_mach_absolute_time();
  uint64_t result;

  if (mach_absolute_last_time) {
    uint64_t delta = current_time - mach_absolute_base_time;
    result = mach_absolute_start_time + (uint64_t)(delta * mach_absolute_time_speed);
    if (result <= mach_absolute_last_time) {
      mach_absolute_base_time = current_time;
      mach_absolute_start_time = mach_absolute_last_time + 1;
      result = mach_absolute_start_time;
    }
  } else {
    mach_absolute_base_time = current_time;
    mach_absolute_start_time = current_time;
    result = current_time;
  }

  mach_absolute_last_time = result;
  os_unfair_lock_unlock(&mach_absolute_base_time_lock);
  return result;
}

int hook_mach_absolute_time(void) {
  if (original_mach_absolute_time) return 0;

  struct rebinding rebindings = { "mach_absolute_time", my_mach_absolute_time_diamond, (void *)&original_mach_absolute_time };
  return rebind_symbols(&rebindings, 1);
}

void set_mach_absolute_time(float value) {
  os_unfair_lock_lock(&mach_absolute_base_time_lock);
  uint64_t current_time = original_mach_absolute_time();
  mach_absolute_base_time = current_time;
  mach_absolute_start_time = mach_absolute_last_time;
  mach_absolute_time_speed = value;
  os_unfair_lock_unlock(&mach_absolute_base_time_lock);
}

void reset_mach_absolute_time(void) { set_mach_absolute_time(1.0); }
