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
LIABILITY, WHETHER IN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

/*
 * CADisplayLink speed mode
 *
 * Principle: swizzle CADisplayLink's `timestamp` and `duration`
 * property getters to return scaled values.
 *
 * Most games compute their frame deltaTime like:
 *   double dt = displayLink.timestamp - lastTimestamp;
 * or:
 *   double dt = displayLink.duration * gameSpeed;
 *
 * By scaling timestamp/duration by speed, the computed dt
 * automatically reflects accelerated time, causing the game
 * to run faster or slower.
 */

#import "LuckySpeeder.h"
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <os/lock.h>

static float g_speed = 1.0;
static BOOL g_hooked = NO;
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;

static CFTimeInterval (*original_timestamp)(CADisplayLink *, SEL) = NULL;
static CFTimeInterval (*original_duration)(CADisplayLink *, SEL) = NULL;
static CFTimeInterval (*original_targetTimestamp)(CADisplayLink *, SEL) = NULL;

static CFTimeInterval my_timestamp(CADisplayLink *self, SEL cmd) {
  CFTimeInterval ts = original_timestamp(self, cmd);

  os_unfair_lock_lock(&g_lock);
  float speed = g_speed;
  os_unfair_lock_unlock(&g_lock);

  return ts * speed;
}

static CFTimeInterval my_duration(CADisplayLink *self, SEL cmd) {
  CFTimeInterval d = original_duration(self, cmd);

  os_unfair_lock_lock(&g_lock);
  float speed = g_speed;
  os_unfair_lock_unlock(&g_lock);

  return d * speed;
}

static CFTimeInterval my_targetTimestamp(CADisplayLink *self, SEL cmd) {
  CFTimeInterval ts = original_targetTimestamp(self, cmd);

  os_unfair_lock_lock(&g_lock);
  float speed = g_speed;
  os_unfair_lock_unlock(&g_lock);

  return ts * speed;
}

int hook_CADisplayLink(void) {
  if (g_hooked) return HOOK_SUCCESS;

  Class cls = [CADisplayLink class];

  Method tsMethod = class_getInstanceMethod(cls, @selector(timestamp));
  Method durMethod = class_getInstanceMethod(cls, @selector(duration));
  if (!tsMethod || !durMethod) return -1;

  original_timestamp = (CFTimeInterval (*)(CADisplayLink *, SEL))method_getImplementation(tsMethod);
  original_duration   = (CFTimeInterval (*)(CADisplayLink *, SEL))method_getImplementation(durMethod);
  method_setImplementation(tsMethod, (IMP)my_timestamp);
  method_setImplementation(durMethod, (IMP)my_duration);

  // targetTimestamp is available on iOS 10+ / tvOS 10+
  Method ttMethod = class_getInstanceMethod(cls, @selector(targetTimestamp));
  if (ttMethod) {
    original_targetTimestamp = (CFTimeInterval (*)(CADisplayLink *, SEL))method_getImplementation(ttMethod);
    method_setImplementation(ttMethod, (IMP)my_targetTimestamp);
  }

  g_hooked = YES;
  return HOOK_SUCCESS;
}

void set_CADisplayLink(float value) {
  os_unfair_lock_lock(&g_lock);
  g_speed = value;
  os_unfair_lock_unlock(&g_lock);
}

void reset_CADisplayLink(void) { set_CADisplayLink(1.0); }
