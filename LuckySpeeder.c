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
#include <sys/time.h>
#include <time.h>

#if !TARGET_OS_TV
#include "hwbphook.h"
#endif

// ---- real clock providers (captured before hwbp hooks go live) ----

static int (*sys_clock_gettime)(clockid_t, struct timespec *);
static int (*sys_gettimeofday)(struct timeval *, void *);
static uint64_t (*sys_mach_absolute_time)(void);
static mach_timebase_info_data_t tb_cache;

static int64_t real_rt_ns(void) {
  struct timeval tv;
  sys_gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000000LL + (int64_t)tv.tv_usec * 1000;
}

static uint64_t real_ticks(void) {
  return sys_mach_absolute_time();
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

#if TARGET_OS_TV
  if (TARGET_OS_TV) {
    struct rebinding rebindings[] = {
      { "clock_gettime", my_clock_gettime, (void *)&sys_clock_gettime },
      { "gettimeofday", my_gettimeofday, (void *)&sys_gettimeofday },
      { "mach_absolute_time", my_mach_absolute_time, (void *)&sys_mach_absolute_time },
    };
    if (rebind_symbols(rebindings, 3) != 0) return -1;
    mach_timebase_info(&tb_cache);
    clock_core_bind(real_ticks, real_rt_ns, tb_cache.numer, tb_cache.denom);
    hooks_installed = 1;
    return 0;
  }
#endif

#if !TARGET_OS_TV
  // capture originals before hwbp redirect
  sys_clock_gettime = dlsym(RTLD_DEFAULT, "clock_gettime");
  sys_gettimeofday = dlsym(RTLD_DEFAULT, "gettimeofday");
  sys_mach_absolute_time = dlsym(RTLD_DEFAULT, "mach_absolute_time");
  if (!sys_clock_gettime || !sys_gettimeofday || !sys_mach_absolute_time)
    return -1;
  mach_timebase_info(&tb_cache);
  clock_core_bind(real_ticks, real_rt_ns, tb_cache.numer, tb_cache.denom);

  void *originals[3] = {
    (void *)sys_clock_gettime,
    (void *)sys_gettimeofday,
    (void *)sys_mach_absolute_time,
  };
  void *hooked[3] = {
    (void *)my_clock_gettime,
    (void *)my_gettimeofday,
    (void *)my_mach_absolute_time,
  };
  if (!hwbp_hook(originals, hooked, 3)) return -1;
  hooks_installed = 1;
#endif // !TARGET_OS_TV
  return 0;
}

void set_clock_gettime(float value) {
  clock_core_set_speed((double)value);
}

void reset_clock_gettime(void) {
  if (!hooks_installed) return;

#if !TARGET_OS_TV
  void *originals[3] = {
    (void *)sys_clock_gettime,
    (void *)sys_gettimeofday,
    (void *)sys_mach_absolute_time,
  };
  hwbp_unhook(originals, 3);
#endif // !TARGET_OS_TV
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
