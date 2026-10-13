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
#include <string>
#include <utility>
#include <vector>

namespace facebook::react {

struct AnimationMutations;

using AnimationTimestamp = std::chrono::duration<double, std::milli>;
using CallbackId = uint64_t;

// A view mounted by a revision with this animation stamp and release
// coverage.
struct StampedView {
  Tag tag;
  uint64_t stamp;
  uint64_t releaseCoverage;
};

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
      const std::vector<StampedView> & /*stampedViews*/)
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
  // The values `props` gives the keys whose owning C++ Animated node was
  // released after `afterReleaseCoverage` and up to `releaseCoverage`.
  virtual folly::dynamic releasedValues(
      SurfaceId /*surfaceId*/,
      Tag /*tag*/,
      uint64_t /*afterReleaseCoverage*/,
      uint64_t /*releaseCoverage*/,
      const Props::Shared & /*props*/)
  {
    return folly::dynamic::object();
  }
  // A C++ Animated props node owns `keys` of the view: nothing else changes
  // them until the node releases them.
  virtual void own(
      const std::shared_ptr<const ShadowNodeFamily> & /*family*/,
      Tag /*propsNodeTag*/,
      const std::vector<std::string> & /*keys*/)
  {
  }
  // Releases the keys the node owns; React's values are then mounted again.
  virtual void release(Tag /*propsNodeTag*/) {}
};

} // namespace facebook::react
