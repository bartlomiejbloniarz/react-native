/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/PrivateGuard.h>

#include <folly/dynamic.h>
#include <optional>
#include <string>
#include "AnimatedProps.h"

namespace facebook::react::animationbackend {

folly::dynamic packAnimatedProps(const AnimatedProps &animatedProps);
// The raw props the typed prop is written as directly; empty for layout props.
folly::dynamic packAnimatedProp(const std::unique_ptr<AnimatedPropBase> &animatedProp);
// The typed prop written directly as exactly this one raw key. Typed props
// written as several keys (border radii, colors, curves and styles) have none.
std::optional<PropName> singleKeyPropName(const std::string &key);
// The typed border prop (radii, colors, curves or styles) whose value includes
// this raw key; assigning it replaces every key of its group.
std::optional<PropName> borderPropName(const std::string &key);
// The typed prop whose value includes this raw key, layout props included.
std::optional<PropName> propNameOfKey(const std::string &key);
// The raw props the typed border prop of `viewProps` is written as.
folly::dynamic packBorderProp(PropName propName, const BaseViewProps &viewProps);
// Whether the props, with their raw props as `rawValues`, set the key: a typed
// border prop sets every key of its group.
bool writesKey(const folly::dynamic *rawValues, const AnimatedProps &props, const std::string &key);

} // namespace facebook::react::animationbackend
