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
  _burstRemaining = _burstLength;
  CFRunLoopSourceSignal(_source);
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

- (void)performBurst
{
  if (!_active || _burstRemaining <= 0) {
    return;
  }
  sPerform++;
  _burstRemaining--;
  CFTimeInterval deadline = CACurrentMediaTime() + _busyMs / 1000.0;
  volatile double x = 0;
  while (CACurrentMediaTime() < deadline) {
    x += 1.0;
    if (x > 1e12) {
      x = 0;
    }
  }
  if (_burstRemaining > 0) {
    CFRunLoopSourceSignal(_source); // keep poll==true next iteration
    CFRunLoopWakeUp(CFRunLoopGetMain());
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

static void RNTDemoPerform(void *info)
{
  [(__bridge RNTDemo *)info performBurst];
}

static void RNTDemoObserver(__unused CFRunLoopObserverRef obs, __unused CFRunLoopActivity act, void *info)
{
  [(__bridge RNTDemo *)info onBeforeWaiting];
}
