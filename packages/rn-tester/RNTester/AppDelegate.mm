/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import "AppDelegate.h"

#if !TARGET_OS_TV
#import <UserNotifications/UserNotifications.h>
#endif

#import <React/RCTBundleURLProvider.h>
#import <React/RCTDefines.h>
#import <React/RCTLinkingManager.h>
#import <ReactCommon/RCTSampleTurboModule.h>
#import <ReactCommon/RCTTurboModuleManager.h>

#if !TARGET_OS_TV
#import <React/RCTPushNotificationManager.h>
#endif

#import <NativeCxxModuleExample/NativeCxxModuleExample.h>
#ifndef RN_DISABLE_OSS_PLUGIN_HEADER
#import <RNTMyNativeViewComponentView.h>
#endif

#if __has_include(<ReactAppDependencyProvider/RCTAppDependencyProvider.h>)
#define USE_OSS_CODEGEN 1
#import <ReactAppDependencyProvider/RCTAppDependencyProvider.h>
#else
#define USE_OSS_CODEGEN 0
#endif

#if RCT_DEV_MENU
#import <React/RCTDevMenu.h>
#endif

static NSString *kBundlePath = @"js/RNTesterApp.ios";

#if !TARGET_OS_TV
@interface AppDelegate () <UNUserNotificationCenterDelegate>
@end
#else
@interface AppDelegate ()
@end
#endif

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
  self.reactNativeFactory = [[RCTReactNativeFactory alloc] initWithDelegate:self];
#if USE_OSS_CODEGEN
  self.dependencyProvider = [RCTAppDependencyProvider new];
#endif

#if RCT_DEV_MENU

  RCTDevMenuConfiguration *devMenuConfiguration = [[RCTDevMenuConfiguration alloc] initWithDevMenuEnabled:true
                                                                                      shakeGestureEnabled:true
                                                                                 keyboardShortcutsEnabled:true];
  [self.reactNativeFactory setDevMenuConfiguration:devMenuConfiguration];

#endif

  self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];

  [self.reactNativeFactory startReactNativeWithModuleName:@"RNTesterApp"
                                                 inWindow:self.window
                                        initialProperties:[self prepareInitialProps]
                                            launchOptions:launchOptions];

#if !TARGET_OS_TV
  [[UNUserNotificationCenter currentNotificationCenter] setDelegate:self];
#endif

  return YES;
}

- (NSDictionary *)prepareInitialProps
{
  NSMutableDictionary *initProps = [NSMutableDictionary new];

  NSString *_routeUri = [[NSUserDefaults standardUserDefaults] stringForKey:@"route"];
  if (_routeUri) {
    initProps[@"exampleFromAppetizeParams"] = [NSString stringWithFormat:@"rntester://example/%@Example", _routeUri];
  }

  return initProps;
}

- (NSURL *)sourceURLForBridge:(RCTBridge *)bridge
{
  return [[RCTBundleURLProvider sharedSettings] jsBundleURLForBundleRoot:kBundlePath];
}

- (BOOL)application:(UIApplication *)app
            openURL:(NSURL *)url
            options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options
{
  return [RCTLinkingManager application:app openURL:url options:options];
}

- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:(const std::string &)name
                                                      jsInvoker:(std::shared_ptr<facebook::react::CallInvoker>)jsInvoker
{
  if (name == facebook::react::NativeCxxModuleExample::kModuleName) {
    return std::make_shared<facebook::react::NativeCxxModuleExample>(jsInvoker);
  }

  return [super getTurboModule:name jsInvoker:jsInvoker];
}

#if !TARGET_OS_TV
// Required for the remoteNotificationsRegistered event.
- (void)application:(__unused UIApplication *)application
    didRegisterForRemoteNotificationsWithDeviceToken:(NSData *)deviceToken
{
  [RCTPushNotificationManager didRegisterForRemoteNotificationsWithDeviceToken:deviceToken];
}

// Required for the remoteNotificationRegistrationError event.
- (void)application:(__unused UIApplication *)application
    didFailToRegisterForRemoteNotificationsWithError:(NSError *)error
{
  [RCTPushNotificationManager didFailToRegisterForRemoteNotificationsWithError:error];
}

#pragma mark - UNUserNotificationCenterDelegate

// Required for the remoteNotificationReceived and localNotificationReceived events
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
  [RCTPushNotificationManager didReceiveNotification:notification];
  completionHandler(UNNotificationPresentationOptionNone);
}

// Required for the remoteNotificationReceived and localNotificationReceived events
// Called when a notification is tapped from background. (Foreground notification will not be shown per
// the presentation option selected above).
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
  UNNotification *notification = response.notification;

  // This condition will be true if tapping the notification launched the app.
  if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
    // This can be retrieved with getInitialNotification.
    [RCTPushNotificationManager setInitialNotification:notification];
  }

  [RCTPushNotificationManager didReceiveNotification:notification];
  completionHandler();
}
#endif

#pragma mark - RCTComponentViewFactoryComponentProvider

#ifndef RN_DISABLE_OSS_PLUGIN_HEADER
- (nonnull NSDictionary<NSString *, Class<RCTComponentViewProtocol>> *)thirdPartyFabricComponents
{
  NSMutableDictionary *dict = [super thirdPartyFabricComponents].mutableCopy;
  if (!dict[@"RNTMyNativeView"]) {
    dict[@"RNTMyNativeView"] = NSClassFromString(@"RNTMyNativeViewComponentView");
  }
  if (!dict[@"SampleNativeComponent"]) {
    dict[@"SampleNativeComponent"] = NSClassFromString(@"RCTSampleNativeComponentComponentView");
  }
  return dict;
}
#endif

- (NSURL *)bundleURL
{
  return [[RCTBundleURLProvider sharedSettings] jsBundleURLForBundleRoot:kBundlePath];
}

@end

#pragma mark - RNTDemo (demo harness)

#import <React/RCTBridgeModule.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <react/featureflags/ReactNativeFeatureFlags.h>
#include <atomic>

extern "C" void RNTSetProbePress(double t); // defined in RCTMountingManager.mm

// Perceived-latency probe. markPress() runs on methodQueue==main, so under the jank it can be
// delayed together with the mount — making timeToMount (their difference) look tiny while the
// user still waits. To measure the part timeToMount can't see (touch -> handler), we swizzle
// -[UIApplication sendEvent:] to stamp the time of the most recent touch-down, then markPress
// logs tapToHandler = now - thatStamp.
static std::atomic<double> gRNTLastTouchDown{0};

// Set by RNTJS.markJS (a module on a BACKGROUND queue, called first from the JS Update
// handler). Lets us split tapToHandler into (touch -> JS handler), which lands here off the
// main thread, vs (JS handler -> main-queue drain), which is markPress running on main.
static std::atomic<double> gRNTJSHandler{0};

@implementation UIApplication (RNTTouchProbe)
- (void)rnt_sendEvent:(UIEvent *)event
{
  if (event.type == UIEventTypeTouches) {
    for (UITouch *t in event.allTouches) {
      if (t.phase == UITouchPhaseBegan) {
        gRNTLastTouchDown.store(CACurrentMediaTime());
        break;
      }
    }
  }
  [self rnt_sendEvent:event]; // swizzled -> original sendEvent:
}
+ (void)load
{
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    method_exchangeImplementations(
        class_getInstanceMethod(self, @selector(sendEvent:)),
        class_getInstanceMethod(self, @selector(rnt_sendEvent:)));
  });
}
@end

@interface RNTDemo : NSObject <RCTBridgeModule>
@end

// source0 starvation trampolines (self-resignaling version-0 run-loop source that keeps
// the main loop in poll==true, suppressing kCFRunLoopBeforeWaiting for several frames).
static void RNTDemoPerform(void *info);
static void RNTDemoObserver(CFRunLoopObserverRef obs, CFRunLoopActivity act, void *info);

@implementation RNTDemo {
  CFRunLoopSourceRef _source;
  CFRunLoopObserverRef _observer;
  BOOL _active;
  NSInteger _burstRemaining;
  NSInteger _burstLength;
  double _busyMs;
  CFTimeInterval _cycleDeadline;
  CADisplayLink *_jankLink;
  double _jankMs;
}

RCT_EXPORT_MODULE(RNTDemo);

- (dispatch_queue_t)methodQueue
{
  return dispatch_get_main_queue(); // so blockMainThread actually blocks the main thread
}

+ (BOOL)requiresMainQueueSetup
{
  return YES;
}

RCT_EXPORT_METHOD(blockMainThread : (double)ms)
{
  usleep((useconds_t)(ms * 1000.0));
}

RCT_EXPORT_METHOD(mark : (NSString *)label)
{
  NSLog(@"[RNTProbe] MARK %@ t=%.4f", label, CACurrentMediaTime());
}

// Records the moment the user pressed "Update" so the mount log can print timeToMount.
RCT_EXPORT_METHOD(markPress)
{
  double now = CACurrentMediaTime();
  RNTSetProbePress(now);
  double td = gRNTLastTouchDown.load();
  double js = gRNTJSHandler.load();
  if (td > 0) {
    // touch-down -> this handler running on main. This is the latency timeToMount can't see.
    // Split: touchToJs (hop 1, touch -> JS handler, lands off-main in RNTJS.markJS) and
    // jsToMain (hop 2, JS handler -> this main-queue method actually running).
    NSLog(@"[RNTProbe] tapToHandler=%.1f ms  touchToJs=%.1f ms  jsToMain=%.1f ms",
          (now - td) * 1000.0,
          js > 0 ? (js - td) * 1000.0 : -1.0,
          js > 0 ? (now - js) * 1000.0 : -1.0);
  }
}

// Lets the JS UI show whether enableFabricCommitBranching is actually on.
RCT_EXPORT_METHOD(getBranching : (RCTResponseSenderBlock)callback)
{
  callback(@[ @(facebook::react::ReactNativeFeatureFlags::enableFabricCommitBranching()) ]);
}

#pragma mark - Jank (CADisplayLink burning busyMs/frame)

// A CADisplayLink that busy-spins for `busyMs` every vsync. Because busyMs > the frame
// interval, the next vsync is always already pending when the tick returns, so the run
// loop grabs it (poll==true) and runs the next tick WITHOUT ever sleeping — i.e. it keeps
// kCFRunLoopBeforeWaiting from firing. This is the realistic "sustained over-budget
// frames" shape (a heavy per-frame animation), not a single synchronous freeze.
- (void)jankTick:(CADisplayLink *)link
{
  CFTimeInterval deadline = CACurrentMediaTime() + _jankMs / 1000.0;
  volatile double x = 0;
  while (CACurrentMediaTime() < deadline) {
    x += 1.0;
    if (x > 1e12) {
      x = 0;
    }
  }
}

RCT_EXPORT_METHOD(startJank : (double)busyMs)
{
  _jankMs = busyMs > 0 ? busyMs : 20.0;
  if (_jankLink == nil) {
    _jankLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(jankTick:)];
    [_jankLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
  }
  _jankLink.paused = NO;
}

RCT_EXPORT_METHOD(stopJank)
{
  _jankLink.paused = YES;
}

#pragma mark - BeforeWaiting starvation (source0)

static NSInteger sBW = 0;
static NSInteger sPerform = 0;
static CFTimeInterval sT0 = 0;

static void RNTDemoTick(void)
{
  CFTimeInterval now = CACurrentMediaTime();
  if (sT0 == 0) {
    sT0 = now;
  }
  if (now - sT0 >= 1.0) {
    NSLog(@"[RNTStarve] BeforeWaiting/s=%ld source0Perform/s=%ld", (long)sBW, (long)sPerform);
    sBW = 0;
    sPerform = 0;
    sT0 = now;
  }
}

- (void)installIfNeeded
{
  if (_source) {
    return;
  }
  CFRunLoopSourceContext sctx;
  memset(&sctx, 0, sizeof(sctx));
  sctx.info = (__bridge void *)self;
  sctx.perform = RNTDemoPerform;
  _source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0 /* version 0 == source0 */, &sctx);
  CFRunLoopAddSource(CFRunLoopGetMain(), _source, kCFRunLoopCommonModes);

  CFRunLoopObserverContext octx;
  memset(&octx, 0, sizeof(octx));
  octx.info = (__bridge void *)self;
  _observer = CFRunLoopObserverCreate(
      kCFAllocatorDefault, kCFRunLoopBeforeWaiting, true, 0, RNTDemoObserver, &octx);
  CFRunLoopAddObserver(CFRunLoopGetMain(), _observer, kCFRunLoopCommonModes);
}

- (void)kickBurst
{
  // Spin (cheaply) for one cycle worth of wall-clock: burstLength * busyMs.
  _cycleDeadline = CACurrentMediaTime() + (_burstLength * _busyMs) / 1000.0;
  CFRunLoopSourceSignal(_source);
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

- (void)performBurst
{
  if (!_active) {
    return;
  }
  sPerform++;
  // Cheap, NON-blocking: re-signal and return. The run loop stays in poll==true
  // (BeforeWaiting suppressed) for the whole cycle, but because we return to the loop on
  // every iteration it still services the touch mach-port and the main-queue dispatch port
  // between re-signals — so input/JS/the dispatch-port mount are NOT starved (only the
  // flag-ON BeforeWaiting merge is). When the cycle deadline passes we stop re-signaling,
  // the loop reaches BeforeWaiting once, and onBeforeWaiting re-arms.
  if (CACurrentMediaTime() < _cycleDeadline) {
    // Yield the core for a sliver so the main thread isn't pegged at 100%. Pegging it (and
    // the old per-iteration CFRunLoopWakeUp flood) added variable latency to touch delivery
    // and the dispatch-port mount even with the flag OFF. 250us is imperceptible but it
    // drops the spin from ~1.2M/s to a few thousand/s. No CFRunLoopWakeUp: the loop is
    // already running (poll==true ⇒ zero-timeout wait, never sleeps), so the signal alone
    // is picked up next iteration; waking it every iteration only floods the mach port.
    usleep(250);
    CFRunLoopSourceSignal(_source); // keep poll==true next iteration
  }
}

- (void)onBeforeWaiting
{
  sBW++;
  RNTDemoTick();
  if (!_active) {
    return;
  }
  [self kickBurst]; // re-arm so starvation is periodic (one BeforeWaiting per cycle)
}

RCT_EXPORT_METHOD(startStarve : (double)burstLength busyMs : (double)busyMs)
{
  _burstLength = burstLength > 0 ? (NSInteger)burstLength : 12;
  _busyMs = busyMs > 0 ? busyMs : 8.0;
  [self installIfNeeded];
  _active = YES;
  [self kickBurst];
}

RCT_EXPORT_METHOD(stopStarve)
{
  _active = NO;
  _burstRemaining = 0;
}

@end

// Background-queue probe: stamps the moment the JS Update handler reached native (off the main
// thread, so it is NOT subject to the jank's main-queue draining). Compared against markPress
// (main queue) this isolates which hop the jank actually delays.
@interface RNTJS : NSObject <RCTBridgeModule>
@end

@implementation RNTJS

RCT_EXPORT_MODULE(RNTJS);

- (dispatch_queue_t)methodQueue
{
  static dispatch_queue_t q;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    q = dispatch_queue_create("com.rntester.jsprobe", DISPATCH_QUEUE_SERIAL);
  });
  return q;
}

RCT_EXPORT_METHOD(markJS)
{
  gRNTJSHandler.store(CACurrentMediaTime());
}

@end

static void RNTDemoPerform(void *info)
{
  [(__bridge RNTDemo *)info performBurst];
}

static void RNTDemoObserver(__unused CFRunLoopObserverRef obs, __unused CFRunLoopActivity act, void *info)
{
  [(__bridge RNTDemo *)info onBeforeWaiting];
}
