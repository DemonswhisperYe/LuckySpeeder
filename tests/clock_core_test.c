// Host-side unit test for clock_core: verifies per-line anchoring and
// speed consistency without any Apple runtime. Build & run:
//   gcc -O2 -o /tmp/cktest tests/clock_core_test.c clock_core.c && /tmp/cktest
#include "../clock_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static uint64_t fake_ticks;
static int64_t  fake_rt;
static uint64_t rt_calls, ns_calls;

static uint64_t t_provider(void) { rt_calls++; return fake_ticks; }
static int64_t  n_provider(void) { ns_calls++; return fake_rt; }

#define NS(x) ((int64_t)(x))
int main(void) {
  const uint64_t TICK = 1; // 1 tick = 1 ns (numer=denom=1)
  clock_core_bind(t_provider, n_provider, 1, 1);

  // ---- monotonic line: no jump on first read, scales afterwards ----
  fake_ticks = 1000;
  assert(clock_core_mach_abs_time() == 1000); // seed

  fake_ticks += 1000; // +1us real
  assert(clock_core_mach_abs_time() == 1000 + 1000); // speed 1.0

  clock_core_set_speed(5.0);
  fake_ticks += 1000;
  uint64_t v = clock_core_mach_abs_time();
  assert(v == 7000); // 2000 + 1000*5

  // ---- realtime line independent of monotonic jumps ----
  fake_rt = NS(1750000000ull * 1000000000ull);
  int64_t r0 = clock_core_realtime_ns();
  assert(r0 == fake_rt); // seed at real wall time

  fake_rt += NS(1000000000ull); // +1s real
  clock_core_set_speed(5.0);
  int64_t r1 = clock_core_realtime_ns();
  assert(r1 == fake_rt + NS(4 * 1000000000ull)); // +1s*5

  // monotonic unaffected by realtime calls (the original bug)
  fake_ticks += 1000;
  uint64_t v2 = clock_core_mach_abs_time();
  assert(v2 == 12000); // 7000 + 1000*5

  // ---- speed change re-anchors: no retroactive jump ----
  clock_core_set_speed(2.0);
  fake_ticks += 100;
  uint64_t v3 = clock_core_mach_abs_time();
  assert(v3 == 12200); // 12000 + 100*2

  // ---- realtime goes backwards must not corrupt monotonic ----
  fake_rt -= NS(5000000000ull);
  int64_t r2 = clock_core_realtime_ns();
  (void)r2;
  fake_ticks += 100;
  uint64_t v4 = clock_core_mach_abs_time();
  assert(v4 == 12400); // 12200 + 100*2

  // ---- non-1:1 timebase scaling ----
  clock_core_bind(t_provider, n_provider, 125, 3); // 1 tick = 41.67ns
  clock_core_reset();
  fake_ticks = 0; fake_rt = NS(1000000000ull);
  assert(clock_core_mach_abs_time() == 0);
  fake_ticks = 24; // 24 ticks = 1000ns elapsed
  uint64_t v5 = clock_core_mach_abs_time();
  assert(v5 == 24); // out in ticks: 1000ns virtual = 24 ticks

  printf("all clock_core tests passed\n");
  return 0;
}
