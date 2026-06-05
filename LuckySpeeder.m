#import "LuckySpeeder.h"
#import <SpriteKit/SpriteKit.h>
#import <objc/runtime.h>

static float SKScene_update_speed = 1.0;

static void (*original_SKScene_update)(id, SEL, NSTimeInterval) = NULL;

static void my_SKScene_update(id self, SEL cmd, NSTimeInterval currentTime) {
  if ([self isKindOfClass:[SKScene class]]) {
    SKScene *scene = (SKScene *)self;
    scene.speed = SKScene_update_speed;
    if (scene.physicsWorld) scene.physicsWorld.speed = SKScene_update_speed;
  }
  if (original_SKScene_update) original_SKScene_update(self, cmd, currentTime);
}

bool classNameHasSuffix(Class cls, const char *suffix) {
  const char *name = class_getName(cls);
  size_t nlen = strlen(name);
  size_t slen = strlen(suffix);
  if (nlen < slen) return false;
  return strcmp(name + nlen - slen, suffix) == 0;
}

int hook_SKScene_update(void) {
  if (original_SKScene_update) return 0;

  int numClasses = objc_getClassList(NULL, 0);
  if (numClasses < 1) return -1;

  unsigned int numClassesUnsigned = (unsigned int)numClasses;

  Class *classes = objc_copyClassList(&numClassesUnsigned);
  if (!classes) return -1;

  Class SKSceneClass = [SKScene class];

  for (unsigned int i = 0; i < numClassesUnsigned; i++) {
    Class cls = classes[i];

    if (class_getSuperclass(cls) != SKSceneClass) continue;

    if (!classNameHasSuffix(cls, "GameScene")) continue;

    Method updateMethod = class_getInstanceMethod(cls, @selector(update:));
    if (updateMethod) {
      original_SKScene_update = (void (*)(id, SEL, NSTimeInterval))method_getImplementation(updateMethod);
      method_setImplementation(updateMethod, (IMP)my_SKScene_update);
      free(classes);
      return 0;
    }
  }

  free(classes);

  return -1;
}

void set_SKScene_update(float value) { SKScene_update_speed = value; }

void reset_SKScene_update(void) { set_SKScene_update(1.0); }

// ── Anti-cheat popup blocker ──

static BOOL anti_popup_hooked = NO;
static void (*orig_alertView_show)(id, SEL) = NULL;
static IMP orig_presentVC = NULL;
// NSDate hook
static IMP orig_NSDate_timeIntervalSinceReferenceDate = NULL;
// MyAlertMsg hook
static IMP orig_MyAlertMsg = NULL;
static Class MyAlertMsgClass = NULL;

static BOOL isAntiCheatAlert(NSString *title, NSString *message) {
  if (!title && !message) return NO;
  NSArray *keywords = @[
    // Chinese
    @"检测到", @"检测到作弊", @"检测到加速", @"检测到修改",
    @"作弊", @"非法", @"异常", @"外挂", @"加速",
    @"速度异常", @"时间异常", @"修改痕迹",
    // English
    @"cheat", @"Cheat", @"CHEAT",
    @"speed", @"Speed hack",
    @"modified", @"hacked",
    @"detected", @"Detection",
    @"third-party", @"unauthorized",
    // Japanese / common
    @"チート", @"不正",
  ];
  for (NSString *kw in keywords) {
    if ([title containsString:kw]) return YES;
    if ([message containsString:kw]) return YES;
  }
  return NO;
}

static void my_alertView_show(id self, SEL cmd) {
  NSString *title = [self title];
  NSString *msg = [self message];
  if (isAntiCheatAlert(title, msg)) {
    return;
  }
  orig_alertView_show(self, cmd);
}

static void my_presentViewController(id self, SEL cmd, id vc, BOOL animated, id completion) {
  // Block anti-cheat alert dialogs
  if ([vc isKindOfClass:[UIAlertController class]]) {
    NSString *title = [vc title];
    NSString *msg  = [vc message];
    if (title || msg) {
      if (isAntiCheatAlert(title, msg)) {
        return;
      }
    }
  }
  // Block SGSSanctionViewController (Stove sanction/ban screen)
  if ([NSStringFromClass([vc class]) containsString:@"SGSSanctionViewController"] ||
      [NSStringFromClass([vc class]) containsString:@"SanctionViewController"]) {
    return;
  }
  ((void (*)(id, SEL, id, BOOL, id))orig_presentVC)(self, cmd, vc, animated, completion);
}

// ── NSDate timeIntervalSinceReferenceDate hook ──

static NSTimeInterval (*orig_date_timeIntervalSinceReferenceDate)(id, SEL) = NULL;

static NSTimeInterval my_NSDate_timeIntervalSinceReferenceDate(id self, SEL cmd) {
  float speed = get_shield_speed();
  if (speed == 1.0f) {
    return ((NSTimeInterval (*)(id, SEL))orig_date_timeIntervalSinceReferenceDate)(self, cmd);
  }

  static NSTimeInterval base_real = 0;
  static NSTimeInterval base_fake = 0;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    base_real = ((NSTimeInterval (*)(id, SEL))orig_date_timeIntervalSinceReferenceDate)(self, cmd);
    base_fake = base_real;
  });

  NSTimeInterval real = ((NSTimeInterval (*)(id, SEL))orig_date_timeIntervalSinceReferenceDate)(self, cmd);
  NSTimeInterval delta = real - base_real;
  NSTimeInterval fake = base_fake + delta * (NSTimeInterval)speed;
  return fake;
}

// ── MyAlertMsg:a_nCode:a_bAppExit: hook ──

static void (*orig_MyAlertMsg_fn)(id, SEL, id, id, BOOL) = NULL;

static void my_MyAlertMsg(id self, SEL cmd, id msg, id code, BOOL appExit) {
  float speed = get_shield_speed();
  if (speed != 1.0f) {
    return;
  }
  orig_MyAlertMsg_fn(self, cmd, msg, code, appExit);
}

int hook_anti_popup(void) {
  if (anti_popup_hooked) return 0;
  anti_popup_hooked = YES;

  // Hook UIAlertView show (old API)
  Class alertView = objc_getClass("UIAlertView");
  if (alertView) {
    Method m = class_getInstanceMethod(alertView, @selector(show));
    if (m) {
      orig_alertView_show = (void (*)(id, SEL))method_getImplementation(m);
      method_setImplementation(m, (IMP)my_alertView_show);
    }
  }

  // Hook UIViewController presentViewController
  Method m = class_getInstanceMethod([UIViewController class],
                                      @selector(presentViewController:animated:completion:));
  if (m) {
    orig_presentVC = method_getImplementation(m);
    method_setImplementation(m, (IMP)my_presentViewController);
  }

  // NSDate timeIntervalSinceReferenceDate swizzle (Shield unified time)
  orig_NSDate_timeIntervalSinceReferenceDate = (void *)class_getMethodImplementation(
      objc_getMetaClass("NSDate"),
      @selector(timeIntervalSinceReferenceDate));
  if (orig_NSDate_timeIntervalSinceReferenceDate) {
    Method nm = class_getClassMethod([NSDate class],
                                      @selector(timeIntervalSinceReferenceDate));
    if (nm) {
      method_setImplementation(nm, (IMP)my_NSDate_timeIntervalSinceReferenceDate);
    }
  }

  // MyAlertMsg:a_nCode:a_bAppExit: dynamic class scan & hook
  int numClasses = objc_getClassList(NULL, 0);
  if (numClasses > 0) {
    int count = numClasses;
    Class *buffer = (Class *)malloc(sizeof(Class) * count);
    objc_getClassList(buffer, (unsigned)count);
    SEL alertSel = @selector(MyAlertMsg:a_nCode:a_bAppExit:);
    for (int i = 0; i < count; i++) {
      Class cls = buffer[i];
      if (!cls) continue;
      Method mm = class_getInstanceMethod(cls, alertSel);
      if (mm) {
        orig_MyAlertMsg_fn = (void (*)(id, SEL, id, id, BOOL))method_getImplementation(mm);
        MyAlertMsgClass = cls;
        method_setImplementation(mm, (IMP)my_MyAlertMsg);
        break;
      }
    }
    free(buffer);
  }

  return 0;
}
