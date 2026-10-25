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

  // fishhook (GOT rebinding) for every target: hw breakpoints turned out
  // to be an exception storm. mach_absolute_time is one of the hottest
  // functions in the process (UIKit/GCD/objc call it thousands of times
  // per frame); each hwbp hit costs a full Mach exception round-trip
  // through the single-threaded mach_msg_server handler, so the queue
  // backs up and the main thread starves (watchdog 0x8BADF00D, log 191544).
  // GOT rebinding costs nothing per call and catches the same call set
  // (every external call in the main binary goes through __stubs/GOT).
  struct rebinding rebindings[3] = {
    { "clock_gettime", my_clock_gettime, (void **)&sys_clock_gettime },
    { "gettimeofday", my_gettimeofday, (void **)&sys_gettimeofday },
    { "mach_absolute_time", my_mach_absolute_time, (void **)&sys_mach_absolute_time },
  };
  if (rebind_symbols(rebindings, 3) != 0) return -1;
  hooks_installed = 1;
  return 0;
}

void set_clock_gettime(float value) {
  clock_core_set_speed((double)value);
}

void reset_clock_gettime(void) {
  if (!hooks_installed) return;

  // rebind back to the real implementations; fishhook rewrites the GOT
  // slots by symbol name, so passing the real addresses restores them
  struct rebinding rebindings[3] = {
    { "clock_gettime", (void *)sys_clock_gettime, NULL },
    { "gettimeofday", (void *)sys_gettimeofday, NULL },
    { "mach_absolute_time", (void *)sys_mach_absolute_time, NULL },
  };
  rebind_symbols(rebindings, 3);
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
