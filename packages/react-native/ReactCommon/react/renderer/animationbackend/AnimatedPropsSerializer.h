/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/PrivateGuard.h>

#include <folly/dynamic.h>
#include "AnimatedProps.h"

namespace facebook::react::animationbackend {

folly::dynamic packAnimatedProps(const AnimatedProps &animatedProps);

void packAnimatedProp(folly::dynamic &dyn, const std::unique_ptr<AnimatedPropBase> &animatedProp);

// Whether a typed prop is packed to a raw prop per edge or corner it sets.
bool packsPerEdge(PropName propName);

// Whether a typed prop is written directly to views, as opposed to layout.
bool isWrittenDirectly(PropName propName);

// The raw props of `viewProps`' value of a typed prop; empty for layout props.
folly::dynamic packViewProp(PropName propName, const BaseViewProps &viewProps);

} // namespace facebook::react::animationbackend
