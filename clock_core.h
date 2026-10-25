#ifndef CLOCK_CORE_H
#define CLOCK_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inject the real clock sources once, before the first scaled read.
//   real_ticks : true monotonic clock in mach ticks (mach_continuous_time)
//   real_rt_ns : true wall clock in ns (CLOCK_REALTIME)
//   numer/denom: mach_timebase_info factors (ticks <-> ns)
void clock_core_bind(uint64_t (*real_ticks)(void),
                     int64_t (*real_rt_ns)(void),
                     uint32_t tb_numer, uint32_t tb_denom);

void clock_core_set_speed(double speed);
double clock_core_speed(void);
void clock_core_reset(void);

// Scaled reads. clock_gettime(CLOCK_MONOTONIC family), mach_absolute_time
// and clock_gettime_nsec_np(monotonic ids) share the monotonic line, which
// runs at `speed`.
//
// clock_core_realtime_ns() deliberately does NOT scale: it returns the real
// wall clock. CLOCK_REALTIME / gettimeofday / time() / CFAbsoluteTime-
// GetCurrent all route through it, and a fast wall clock is directly
// observable by the server as timestamp drift.
uint64_t clock_core_mach_abs_time(void);   // ticks (monotonic line, scaled)
int64_t  clock_core_realtime_ns(void);     // ns (real wall clock, unscaled)
uint64_t clock_core_ns_from_ticks(uint64_t ticks); // ticks -> ns, cached timebase

#ifdef __cplusplus
}
#endif
#endif
