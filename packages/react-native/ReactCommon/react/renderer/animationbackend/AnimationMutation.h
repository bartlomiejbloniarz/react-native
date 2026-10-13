/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_set>

#include <react/cxxstableapi/FrameworksGuard.h>

#include <React/RendererCore.h>
#include <memory>
#include <set>
#include <vector>
#include "AnimatedProps.h"

namespace facebook::react {

struct AnimationMutation {
  Tag tag;
  std::shared_ptr<const ShadowNodeFamily> family;
  AnimatedProps props;
  bool hasLayoutUpdates{false};
  // The raw props keys whose animation finished with this write.
  std::unordered_set<std::string> finishedKeys;
};

struct AnimationMutations {
  std::vector<AnimationMutation> batch;
  std::set<SurfaceId> asyncFlushSurfaces;
};

} // namespace facebook::react
