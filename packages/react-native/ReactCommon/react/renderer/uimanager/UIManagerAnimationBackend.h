/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/FrameworksGuard.h>

#include <React/CallInvoker.h>
#include <React/RendererCore.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace facebook::react {

struct AnimationMutations;

using AnimationTimestamp = std::chrono::duration<double, std::milli>;
using CallbackId = uint64_t;
using AnimationFrontendId = uint32_t;

// A producer of animation mutations, such as C++ Animated or Reanimated.
struct AnimationFrontend {
  // Whether the frontend's direct (non-layout) values are written into React
  // commits. Either way a frontend's values win until it releases them.
  bool commitDirectProps{true};
  // Runs every frame while the frontend is started, after the callbacks of
  // frontends registered earlier.
  std::function<AnimationMutations(AnimationTimestamp)> onAnimationFrame;
};

// Keys of a view the frontend stops animating. A key of a typed group (border
// radii, colors, curves and styles; margin, padding, position and border width
// edges) releases the whole group, and the frontend that wrote any of its keys
// last owns it.
struct AnimatedPropsRelease {
  std::shared_ptr<const ShadowNodeFamily> family;
  std::vector<std::string> keys;
  // React adopts the animated values instead of showing its own again.
  bool commitValues{false};
  // The values to show instead, null to reset, for keys written into React
  // commits: React's own values are no longer known there. Applied only to the
  // keys this frontend still owns; otherwise such keys keep the animated value.
  folly::dynamic restoredValues = folly::dynamic::object();
};

class UIManagerAnimationBackend {
 public:
  using Callback = std::function<AnimationMutations(AnimationTimestamp)>;

  virtual ~UIManagerAnimationBackend() = default;

  virtual void onAnimationFrame(AnimationTimestamp timestamp) = 0;
  virtual AnimationFrontendId registerFrontend(AnimationFrontend frontend) = 0;
  // Starting a started frontend or stopping a stopped one does nothing.
  virtual void start(AnimationFrontendId frontendId) = 0;
  virtual void stop(AnimationFrontendId frontendId) = 0;
  virtual void pushAnimationMutations(AnimationFrontendId frontendId, const Callback &callback) = 0;
  // Releases keys only while this frontend wrote their latest values.
  virtual void release(AnimationFrontendId frontendId, std::vector<AnimatedPropsRelease> releases) = 0;
  // Deprecated, for producers that do not register (older Reanimated, third
  // parties). They share one frontend: its direct values are written into
  // React commits until a React commit or an end sync clears them, and its
  // mutations' asyncFlushSurfaces request end syncs.
  virtual CallbackId start(const Callback &callback) = 0;
  virtual void pushAnimationMutations(const Callback &callback) = 0;
  virtual void stop(CallbackId callbackId) = 0;
  virtual void clearRegistry(SurfaceId surfaceId) = 0;
  virtual void clearRegistryOnSurfaceStop(SurfaceId surfaceId) = 0;
  virtual void trigger() = 0;
  virtual void registerJSInvoker(std::shared_ptr<CallInvoker> jsInvoker) = 0;
};

} // namespace facebook::react
