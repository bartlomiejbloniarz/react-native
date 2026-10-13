/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/FrameworksGuard.h>

#include <React/RendererCore.h>
#include <react/renderer/mounting/ShadowViewMutation.h>

#include <optional>

namespace facebook::react {

/*
 * Calculates a list of view mutations which describes how the old
 * `ShadowTree` can be transformed to the new one.
 * The list of mutations might be and might not be optimal.
 */
ShadowViewMutation::List calculateShadowViewMutations(
    const ShadowNode &oldRootShadowNode,
    const ShadowNode &newRootShadowNode);

struct MountedShadowView {
  Tag parentTag;
  ShadowView shadowView;
};

/*
 * The view of `family` that the mutations calculated for the tree of
 * `rootShadowNode` mount, and the tag of the view it is mounted in; none when
 * the node is flattened, culled, hidden or not in the tree.
 */
std::optional<MountedShadowView> findMountedShadowView(
    const ShadowNode &rootShadowNode,
    const ShadowNodeFamily &family);

} // namespace facebook::react
