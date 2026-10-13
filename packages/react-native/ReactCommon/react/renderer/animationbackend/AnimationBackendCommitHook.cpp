/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/animationbackend/AnimationBackendCommitHook.h>

#include <utility>

namespace facebook::react {

namespace {

// The snapshot's raw props with its directly written typed props, which
// platforms mounting raw props apply from the raw props.
folly::dynamic rawPropsOf(const PropsSnapshot& propsSnapshot) {
  auto rawProps = propsSnapshot.rawProps ? *propsSnapshot.rawProps
                                         : folly::dynamic::object();
  for (auto propName : propsSnapshot.propNames) {
    if (animationbackend::isWrittenDirectly(propName)) {
      rawProps.update(
          animationbackend::packViewProp(propName, *propsSnapshot.props));
    }
  }
  return rawProps;
}

} // namespace

AnimationBackendCommitHook::AnimationBackendCommitHook(
    UIManager& uiManager,
    std::shared_ptr<AnimatedPropsRegistry> animatedPropsRegistry)
    : animatedPropsRegistry_(std::move(animatedPropsRegistry)) {
  uiManager.registerCommitHook(*this);
}

RootShadowNode::Unshared AnimationBackendCommitHook::shadowTreeWillCommit(
    const ShadowTree& shadowTree,
    const RootShadowNode::Shared& oldRootShadowNode,
    const RootShadowNode::Unshared& newRootShadowNode,
    const ShadowTreeCommitOptions& commitOptions) noexcept {
  if (commitOptions.source != ShadowTreeCommitSource::React &&
      commitOptions.source != ShadowTreeCommitSource::AnimationEndSync &&
      commitOptions.source != ShadowTreeCommitSource::ReactRevisionMerge) {
    return newRootShadowNode;
  }

  auto snapshot =
      animatedPropsRegistry_->takeStampingSnapshot(shadowTree.getSurfaceId());
  if (auto* telemetry = TransactionTelemetry::threadLocalTelemetry()) {
    telemetry->setAnimationStampNumber(snapshot.number);
  }
  const auto& surfaceFamilies = snapshot.families;
  const auto& updates = snapshot.props;

  if (surfaceFamilies.empty()) {
    return newRootShadowNode;
  }
  auto clonedRootShadowNode =
      std::static_pointer_cast<RootShadowNode>(newRootShadowNode->cloneMultiple(
          surfaceFamilies,
          [&surfaceFamilies, &updates](
              const ShadowNode& shadowNode,
              const ShadowNodeFragment& fragment) {
            auto newProps = ShadowNodeFragment::propsPlaceholder();
            std::shared_ptr<BaseViewProps> viewProps = nullptr;
            if (auto updatesIter = updates.find(shadowNode.getTag());
                updatesIter != updates.end() &&
                surfaceFamilies.contains(shadowNode.getFamilyShared())) {
              const auto& propsSnapshot = updatesIter->second;
              if (!propsSnapshot.propNames.empty() || propsSnapshot.rawProps) {
                PropsParserContext propsParserContext{
                    shadowNode.getSurfaceId(),
                    *shadowNode.getContextContainer()};
                newProps = shadowNode.getComponentDescriptor().cloneProps(
                    propsParserContext,
                    shadowNode.getProps(),
                    RawProps(rawPropsOf(propsSnapshot)));
                viewProps = std::const_pointer_cast<BaseViewProps>(
                    std::static_pointer_cast<const BaseViewProps>(newProps));
              }

              for (const auto& propName : propsSnapshot.propNames) {
                if (auto edgesIt = propsSnapshot.layoutEdges.find(propName);
                    edgesIt != propsSnapshot.layoutEdges.end()) {
                  for (auto edge : edgesIt->second) {
                    updateLayoutEdge(
                        propName, edge, *viewProps, *propsSnapshot.props);
                  }
                } else {
                  updateProp(propName, *viewProps, propsSnapshot);
                }
              }
            }
            return shadowNode.clone(
                {.props = newProps,
                 .children = fragment.children,
                 .state = shadowNode.getState(),
                 .runtimeShadowNodeReference = true});
          }));

  if (clonedRootShadowNode == nullptr) {
    return newRootShadowNode;
  }

  return clonedRootShadowNode;
}

} // namespace facebook::react
