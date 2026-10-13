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
#include <folly/dynamic.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace facebook::react {

struct AnimationMutations;

using AnimationTimestamp = std::chrono::duration<double, std::milli>;
using CallbackId = uint64_t;

class UIManagerAnimationBackend {
 public:
  using Callback = std::function<AnimationMutations(AnimationTimestamp)>;

  virtual ~UIManagerAnimationBackend() = default;

  virtual void onAnimationFrame(AnimationTimestamp timestamp) = 0;
  virtual CallbackId start(const Callback &callback) = 0;
  virtual void stop(CallbackId callbackId) = 0;
  virtual void clearRegistry(SurfaceId surfaceId) = 0;
  virtual void clearRegistryOnSurfaceStop(SurfaceId surfaceId) = 0;
  virtual void trigger() = 0;
  virtual void pushAnimationMutations(const Callback &callback) = 0;
  virtual void registerJSInvoker(std::shared_ptr<CallInvoker> jsInvoker) = 0;
  // For each view and the animation stamp number of a mount, the props the
  // backend wrote directly to the view after that stamp, which win over the
  // mount's values; empty when the surface has no direct writes.
  virtual std::vector<folly::dynamic> newerDirectWrites(
      SurfaceId /*surfaceId*/,
      const std::vector<std::pair<Tag, uint64_t>> & /*stampedViews*/)
  {
    return {};
  }
  virtual bool hasDirectWrites(SurfaceId /*surfaceId*/)
  {
    return false;
  }
  // The values `props` gives the props written directly to the view, where
  // they differ from the written values or from the props the view was
  // preallocated with (default props when it was not), either of which the
  // native view may still show.
  virtual folly::dynamic restoreDirectWrites(
      SurfaceId /*surfaceId*/,
      Tag /*tag*/,
      const Props::Shared & /*props*/,
      const Props::Shared & /*preallocatedProps*/)
  {
    return folly::dynamic::object();
  }
};

} // namespace facebook::react
