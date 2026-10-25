// Unified virtual clock. The monotonic line is scaled; the realtime line
// is NOT.
//
// Why realtime must stay real: the server owns a true wall clock. Any
// timestamp the client derives from CLOCK_REALTIME / gettimeofday / time()
// / CFAbsoluteTimeGetCurrent can be compared against it, so accelerating
// those exposes the speed factor as drift of (speed-1)*elapsed. The game's
// own frame clock is monotonic (mach_absolute_time and
// clock_gettime_nsec_np(CLOCK_HIGHRES)), which no server can cross-check.
//
// History: an earlier version put both lines on one speed and tripped the
// anti-cheat's virtual-time detector (RHB_TM_E_VTD_*); before that, a
// shared accumulator mixed CLOCK_REALTIME (~1.75e9 s) with CLOCK_MONOTONIC
// (~1e5 s) and pushed wall time centuries ahead.
#include "clock_core.h"

#ifdef __APPLE__
#include <os/lock.h>
#define CK_LOCK os_unfair_lock
#define CK_LOCK_INIT OS_UNFAIR_LOCK_INIT
#define CK_LOCK_LCK(l) os_unfair_lock_lock(&(l))
#define CK_LOCK_UNLCK(l) os_unfair_lock_unlock(&(l))
#else
// host-test fallback; on Apple platforms os_unfair_lock is always used
#include <pthread.h>
#define CK_LOCK pthread_mutex_t
#define CK_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define CK_LOCK_LCK(l) pthread_mutex_lock(&(l))
#define CK_LOCK_UNLCK(l) pthread_mutex_unlock(&(l))
#endif

static CK_LOCK lock = CK_LOCK_INIT;

static uint64_t (*real_ticks)(void);
static int64_t (*real_rt_ns)(void);
static uint32_t tb_numer = 1, tb_denom = 1;

static double speed = 1.0;

typedef struct {
  uint64_t virt;   // last virtual reading (ticks for mono, ns for rt)
  uint64_t real;   // real reading at last call
  uint8_t  primed;
} line_t;

static line_t mono = {0};

void clock_core_bind(uint64_t (*rticks)(void), int64_t (*rtns)(void),
                     uint32_t numer, uint32_t denom) {
  CK_LOCK_LCK(lock);
  real_ticks = rticks;
  real_rt_ns = rtns;
  tb_numer = numer ? numer : 1;
  tb_denom = denom ? denom : 1;
  mono.primed = 0;
  CK_LOCK_UNLCK(lock);
}

void clock_core_set_speed(double s) {
  if (s <= 0) s = 1.0;
  CK_LOCK_LCK(lock);
  speed = s; // takes effect from the next read; pending deltas are tiny
  CK_LOCK_UNLCK(lock);
}

double clock_core_speed(void) {
  CK_LOCK_LCK(lock);
  double s = speed;
  CK_LOCK_UNLCK(lock);
  return s;
}

void clock_core_reset(void) { clock_core_set_speed(1.0); }

static inline uint64_t ticks_to_ns(uint64_t t);

uint64_t clock_core_ns_from_ticks(uint64_t t) {
  return ticks_to_ns(t);
}

static inline uint64_t ticks_to_ns(uint64_t t) {
  // no 128-bit needed: numer/denom fit 32 bits, use __int128 multiply
  return (uint64_t)(((__uint128_t)t * tb_numer) / tb_denom);
}

static inline uint64_t ns_to_ticks(uint64_t ns) {
  return (uint64_t)(((__uint128_t)ns * tb_denom) / tb_numer);
}

// Advance one line: virt += (now_ns - last_ns) * speed. Internal unit: ns.
// A backwards/jumping real clock re-anchors instead of accumulating.
static uint64_t advance_ns(line_t *l, uint64_t now_ns, double sp) {
  uint64_t last_ns = l->real; // l->real is already in ns for both lines
  uint64_t delta = now_ns - last_ns; // wraps on backwards jump -> huge
  l->real = now_ns;
  if (delta > (1ULL << 62)) { // backwards jump: re-anchor, no accumulate
    return l->virt;
  }
  l->virt += (uint64_t)(delta * sp);
  return l->virt;
}

uint64_t clock_core_mach_abs_time(void) {
  CK_LOCK_LCK(lock);
  uint64_t real_now = real_ticks();
  uint64_t now_ns = ticks_to_ns(real_now);
  if (!mono.primed) {
    mono.real = now_ns;                 // l->real unit: ns
    mono.virt = now_ns;
    mono.primed = 1;
  } else {
    advance_ns(&mono, now_ns, speed);
  }
  uint64_t out = ns_to_ticks(mono.virt); // convert back to ticks
  CK_LOCK_UNLCK(lock);
  return out;
}

// Realtime reads pass through untouched: scaling the wall clock is the
// one thing a server can always detect, and the game does not need it for
// speed (frame timing is monotonic).
int64_t clock_core_realtime_ns(void) {
  CK_LOCK_LCK(lock);
  int64_t now = real_rt_ns();
  CK_LOCK_UNLCK(lock);
  return now;
}
