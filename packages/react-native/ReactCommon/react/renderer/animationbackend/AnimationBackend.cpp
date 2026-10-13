/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnimationBackend.h"
#include "AnimatedPropsRegistry.h"
#include "AnimationBackendCommitHook.h"

#include <react/debug/react_native_assert.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/graphics/Color.h>
#include <algorithm>
#include <chrono>
#include <utility>

namespace facebook::react {

static inline Props::Shared cloneProps(
    AnimatedProps& animatedProps,
    const ShadowNode& shadowNode) {
  PropsParserContext propsParserContext{
      shadowNode.getSurfaceId(), *shadowNode.getContextContainer()};
  Props::Shared newProps;
  if (animatedProps.rawProps) {
    if (ReactNativeFeatureFlags::enableFabricCommitBranching()) {
      newProps = shadowNode.getComponentDescriptor().cloneProps(
          propsParserContext,
          shadowNode.getProps(),
          std::move(*animatedProps.rawProps));
    } else {
      newProps = shadowNode.getComponentDescriptor().cloneProps(
          propsParserContext,
          shadowNode.getProps(),
          RawProps(*animatedProps.rawProps));
    }
  } else {
    newProps = shadowNode.getComponentDescriptor().cloneProps(
        propsParserContext, shadowNode.getProps(), {});
  }

  auto viewProps = std::const_pointer_cast<BaseViewProps>(
      std::static_pointer_cast<const BaseViewProps>(newProps));
  for (auto& animatedProp : animatedProps.props) {
    cloneProp(*viewProps, *animatedProp);
  }
  return newProps;
}

static void mergeMutation(
    AnimationMutation& existing,
    AnimationMutation&& incoming) {
  auto& props = existing.props;
  for (auto& animatedProp : incoming.props.props) {
    props.props.push_back(std::move(animatedProp));
  }
  if (incoming.props.rawProps) {
    if (props.rawProps) {
      auto merged = props.rawProps->toDynamic();
      mergeAnimatedRawProps(merged, incoming.props.rawProps->toDynamic());
      props.rawProps = std::make_unique<RawProps>(std::move(merged));
    } else {
      props.rawProps = std::move(incoming.props.rawProps);
    }
  }
  existing.hasLayoutUpdates |= incoming.hasLayoutUpdates;
  existing.finishedKeys.merge(incoming.finishedKeys);
}

// Typed props set per edge or corner become raw props, which, like the
// direct writes, set only the edges they contain.
static void movePerEdgePropsToRawProps(AnimatedProps& animatedProps) {
  folly::dynamic perEdgeProps = folly::dynamic::object();
  std::erase_if(animatedProps.props, [&](const auto& animatedProp) {
    if (!animationbackend::packsPerEdge(animatedProp->propName)) {
      return false;
    }
    animationbackend::packAnimatedProp(perEdgeProps, animatedProp);
    return true;
  });
  if (perEdgeProps.empty()) {
    return;
  }
  auto rawProps = animatedProps.rawProps ? animatedProps.rawProps->toDynamic()
                                         : folly::dynamic::object();
  mergeAnimatedRawProps(rawProps, perEdgeProps);
  animatedProps.rawProps = std::make_unique<RawProps>(std::move(rawProps));
}

AnimationBackend::AnimationBackend(
    std::shared_ptr<AnimationChoreographer> animationChoreographer,
    std::shared_ptr<UIManager> uiManager)
    : animatedPropsRegistry_(std::make_shared<AnimatedPropsRegistry>()),
      animationChoreographer_(std::move(animationChoreographer)),
      commitHook_(
          std::make_unique<AnimationBackendCommitHook>(
              *uiManager,
              animatedPropsRegistry_)),
      uiManager_(std::move(uiManager)) {
  react_native_assert(uiManager_.expired() == false);

  auto weakAnimatedPropsRegistry =
      std::weak_ptr<AnimatedPropsRegistry>(animatedPropsRegistry_);
  auto initializeSurfaceContext =
      [weakAnimatedPropsRegistry](const ShadowTree& shadowTree) {
        if (auto animatedPropsRegistry = weakAnimatedPropsRegistry.lock()) {
          animatedPropsRegistry->initializeSurface(shadowTree.getSurfaceId());
          shadowTree.getMountingCoordinator()->setTransactionFinalizer(
              animatedPropsRegistry);
        }
      };

  if (auto lockedUIManager = uiManager_.lock()) {
    lockedUIManager->addOnSurfaceStartCallback(initializeSurfaceContext);
    lockedUIManager->getShadowTreeRegistry().enumerate(
        [&](const ShadowTree& shadowTree, bool& /*stop*/) {
          initializeSurfaceContext(shadowTree);
        });
  }
}

AnimationBackend::~AnimationBackend() = default;

void AnimationBackend::unpackMutations(
    AnimationMutations& mutations,
    std::unordered_map<SurfaceId, SurfaceUpdates>& surfaceUpdates,
    std::set<SurfaceId>& asyncFlushSurfaces) {
  for (auto& mutation : mutations.batch) {
    auto& updates = surfaceUpdates[mutation.family->getSurfaceId()];
    const auto tag = mutation.tag;
    if (auto it = updates.find(tag); it != updates.end()) {
      mergeMutation(it->second, std::move(mutation));
    } else {
      updates.emplace(tag, std::move(mutation));
    }
  }

  asyncFlushSurfaces.merge(mutations.asyncFlushSurfaces);
}

void AnimationBackend::applySurfaceUpdates(
    std::unordered_map<SurfaceId, SurfaceUpdates>& surfaceUpdates,
    const std::set<SurfaceId>& asyncFlushSurfaces) {
  for (auto& [surfaceId, updates] : surfaceUpdates) {
    SurfaceUpdates layoutUpdates;
    std::unordered_map<Tag, AnimatedProps> directProps;
    for (auto& [tag, mutation] : updates) {
      if (mutation.hasLayoutUpdates) {
        // Committed views are written directly too, so typed props reach
        // platforms that mount raw props.
        directProps.emplace(
            tag,
            AnimatedProps{
                .props = {},
                .rawProps = std::make_unique<RawProps>(
                    animationbackend::packAnimatedProps(mutation.props))});
        // Owned props stay out of the shadow tree, which keeps React's values
        // for when they are released.
        animatedPropsRegistry_->removeOwnedProps(
            surfaceId, tag, mutation.props);
        layoutUpdates.emplace(tag, std::move(mutation));
      } else {
        directProps.emplace(tag, std::move(mutation.props));
      }
    }
    if (!directProps.empty()) {
      synchronouslyUpdateProps(directProps);
    }
    if (!layoutUpdates.empty()) {
      commitUpdates(surfaceId, layoutUpdates);
    }
  }

  requestAsyncFlushForSurfaces(asyncFlushSurfaces);
}

void AnimationBackend::applyMutations(std::vector<AnimationMutations> batches) {
  requestReleaseCommit();
  std::unordered_map<SurfaceId, SurfaceUpdates> surfaceUpdates;
  std::set<SurfaceId> asyncFlushSurfaces;
  for (auto& mutations : batches) {
    unpackMutations(mutations, surfaceUpdates, asyncFlushSurfaces);
  }
  for (auto& [_, updates] : surfaceUpdates) {
    for (auto& [__, mutation] : updates) {
      movePerEdgePropsToRawProps(mutation.props);
    }
  }
  animatedPropsRegistry_->update(surfaceUpdates);
  applySurfaceUpdates(surfaceUpdates, asyncFlushSurfaces);
}

void AnimationBackend::onAnimationFrame(AnimationTimestamp timestamp) {
  std::vector<CallbackWithId> callbacksCopy;

  {
    std::lock_guard lock(mutex_);
    callbacksCopy = callbacks;
  }

  // Sized up front rather than grown: MSVC's std::set move isn't noexcept, so
  // growing a vector of AnimationMutations would try to copy move-only props.
  std::vector<AnimationMutations> batches(callbacksCopy.size());
  std::transform(
      callbacksCopy.begin(),
      callbacksCopy.end(),
      batches.begin(),
      [timestamp](const CallbackWithId& callbackWithId) {
        return callbackWithId.callback(timestamp);
      });
  applyMutations(std::move(batches));
}

CallbackId AnimationBackend::start(const Callback& callback) {
  std::lock_guard lock(mutex_);

  auto callbackId = nextCallbackId_++;
  callbacks.push_back({.callbackId = callbackId, .callback = callback});
  if (!isRenderCallbackStarted_) {
    animationChoreographer_->resume();
    isRenderCallbackStarted_ = true;
  }

  return callbackId;
}

void AnimationBackend::stop(CallbackId callbackId) {
  std::lock_guard lock(mutex_);

  auto it = std::find_if(callbacks.begin(), callbacks.end(), [&](auto& c) {
    return c.callbackId == callbackId;
  });
  if (it == callbacks.end()) {
    return;
  }

  callbacks.erase(it);
  if (isRenderCallbackStarted_ && callbacks.empty()) {
    animationChoreographer_->pause();
    isRenderCallbackStarted_ = false;
  }
}

void AnimationBackend::trigger() {
  onAnimationFrame(std::chrono::steady_clock::now().time_since_epoch());
}

void AnimationBackend::pushAnimationMutations(const Callback& callback) {
  auto timestamp = animationChoreographer_->now();
  std::vector<AnimationMutations> batches(1);
  batches[0] = callback(timestamp);
  applyMutations(std::move(batches));
}

void AnimationBackend::commitUpdates(
    SurfaceId surfaceId,
    SurfaceUpdates& surfaceUpdates) {
  auto uiManager = uiManager_.lock();
  if (!uiManager) {
    return;
  }

  std::unordered_set<std::shared_ptr<const ShadowNodeFamily>> surfaceFamilies;
  for (const auto& [tag, mutation] : surfaceUpdates) {
    surfaceFamilies.insert(mutation.family);
  }

  uiManager->getShadowTreeRegistry().visit(
      surfaceId,
      [&surfaceFamilies, &surfaceUpdates](const ShadowTree& shadowTree) {
        shadowTree.commit(
            [&surfaceFamilies,
             &surfaceUpdates](const RootShadowNode& oldRootShadowNode) {
              return std::static_pointer_cast<RootShadowNode>(
                  oldRootShadowNode.cloneMultiple(
                      surfaceFamilies,
                      [&surfaceFamilies, &surfaceUpdates](
                          const ShadowNode& shadowNode,
                          const ShadowNodeFragment& fragment) {
                        auto newProps = ShadowNodeFragment::propsPlaceholder();
                        if (surfaceFamilies.contains(
                                shadowNode.getFamilyShared())) {
                          auto& animatedProps =
                              surfaceUpdates.at(shadowNode.getTag()).props;
                          newProps = cloneProps(animatedProps, shadowNode);
                        }
                        return shadowNode.clone(
                            {.props = newProps,
                             .children = fragment.children,
                             .state = shadowNode.getState()});
                      }));
            },
            {.mountSynchronously = true});
      });
}

void AnimationBackend::synchronouslyUpdateProps(
    const std::unordered_map<Tag, AnimatedProps>& updates) {
  if (auto uiManager = uiManager_.lock()) {
    uiManager->synchronouslyUpdateAnimatedProps(updates);
  }
}

void AnimationBackend::requestAsyncFlushForSurfaces(
    const std::set<SurfaceId>& surfaces) {
  react_native_assert(
      jsInvoker_ != nullptr ||
      surfaces.empty() && "jsInvoker_ was not provided");
  std::weak_ptr<AnimatedPropsRegistry> weakAnimatedPropsRegistry =
      animatedPropsRegistry_;
  for (const auto& surfaceId : surfaces) {
    // perform an empty commit on the js thread, to force the commit hook to
    // push updated shadow nodes to react through RSNRU
    jsInvoker_->invokeAsync(
        [weakUIManager = uiManager_, surfaceId, weakAnimatedPropsRegistry]() {
          auto uiManager = weakUIManager.lock();
          if (!uiManager) {
            return;
          }
          uiManager->getShadowTreeRegistry().visit(
              surfaceId,
              [weakAnimatedPropsRegistry](const ShadowTree& shadowTree) {
                auto result = shadowTree.commit(
                    [weakAnimatedPropsRegistry](
                        const RootShadowNode& oldRootShadowNode) {
                      return std::static_pointer_cast<RootShadowNode>(
                          oldRootShadowNode.ShadowNode::clone({}));
                    },
                    {.source = ShadowTreeCommitSource::AnimationEndSync});
                // To clear the registry, the updates neeed to be propagated to
                // React with RSNRU. Without
                // updateRuntimeShadowNodeReferencesOnCommitThread this won't
                // happen if we do any commits on the main thread, since the
                // runtimeShadowNodeReference_ is not propagated to nodes cloned
                // outside of the JS thread. So when the flag is disabled we
                // keep the updates in the registry and we will reapply them in
                // a commit hook triggered by a rerender.
                if (result == ShadowTree::CommitStatus::Succeeded &&
                    ReactNativeFeatureFlags::
                        updateRuntimeShadowNodeReferencesOnCommitThread()) {
                  if (auto animatedPropsRegistry =
                          weakAnimatedPropsRegistry.lock()) {
                    animatedPropsRegistry->clear(shadowTree.getSurfaceId());
                  }
                }
              });
        });
  }
}

std::vector<folly::dynamic> AnimationBackend::newerDirectWrites(
    SurfaceId surfaceId,
    const std::vector<StampedView>& stampedViews) {
  return animatedPropsRegistry_->newerDirectWrites(surfaceId, stampedViews);
}

void AnimationBackend::own(
    const std::shared_ptr<const ShadowNodeFamily>& family,
    Tag propsNodeTag,
    const std::vector<std::string>& keys) {
  animatedPropsRegistry_->own(family, propsNodeTag, keys);
}

void AnimationBackend::release(Tag propsNodeTag) {
  if (auto connection = animatedPropsRegistry_->disconnect(propsNodeTag)) {
    std::lock_guard lock(releasesMutex_);
    releasedConnections_.push_back(*connection);
  }
}

void AnimationBackend::requestReleaseCommit() {
  std::vector<uint64_t> connections;
  {
    std::lock_guard lock(releasesMutex_);
    connections.swap(releasedConnections_);
  }
  if (connections.empty() || jsInvoker_ == nullptr) {
    return;
  }
  // Releases are committed on the JS thread, in order with React and end
  // sync commits.
  jsInvoker_->invokeAsync(
      [weakThis = weak_from_this(), connections = std::move(connections)]() {
        if (auto self = weakThis.lock()) {
          self->commitPendingReleases();
          self->commitReleases(connections);
          self->stopReleaseRetriesIfIdle();
        }
      });
}

void AnimationBackend::commitReleases(
    const std::vector<uint64_t>& connections) {
  for (auto surfaceId : animatedPropsRegistry_->surfacesOf(connections)) {
    auto [releaseNumber, families] =
        animatedPropsRegistry_->releaseOnSurface(surfaceId, connections);
    if (!families.empty()) {
      commitRelease(
          {.surfaceId = surfaceId,
           .releaseNumber = releaseNumber,
           .families = {families.begin(), families.end()}});
    }
  }
}

void AnimationBackend::commitPendingReleases() {
  std::vector<ReleaseCommit> pendingCommits;
  {
    std::lock_guard lock(releasesMutex_);
    pendingCommits.swap(pendingReleaseCommits_);
  }
  for (const auto& releaseCommit : pendingCommits) {
    commitRelease(releaseCommit);
  }
}

void AnimationBackend::requestReleaseRetry() {
  {
    std::lock_guard lock(releasesMutex_);
    if (isReleaseRetryRequested_ || pendingReleaseCommits_.empty()) {
      return;
    }
    isReleaseRetryRequested_ = true;
  }
  jsInvoker_->invokeAsync([weakThis = weak_from_this()]() {
    if (auto self = weakThis.lock()) {
      {
        std::lock_guard lock(self->releasesMutex_);
        self->isReleaseRetryRequested_ = false;
      }
      self->commitPendingReleases();
      self->stopReleaseRetriesIfIdle();
    }
  });
}

void AnimationBackend::stopReleaseRetriesIfIdle() {
  std::lock_guard lock(releasesMutex_);
  if (pendingReleaseCommits_.empty() && releaseRetryCallbackId_) {
    stop(*std::exchange(releaseRetryCallbackId_, std::nullopt));
  }
}

void AnimationBackend::commitRelease(const ReleaseCommit& releaseCommit) {
  auto uiManager = uiManager_.lock();
  if (!uiManager) {
    return;
  }
  const auto& [surfaceId, releaseNumber, weakFamilies] = releaseCommit;
  std::vector<std::shared_ptr<const ShadowNodeFamily>> families;
  for (const auto& weakFamily : weakFamilies) {
    if (auto family = weakFamily.lock()) {
      families.push_back(std::move(family));
    }
  }
  if (families.empty()) {
    return;
  }
  auto status = ShadowTree::CommitStatus::Succeeded;
  uiManager->getShadowTreeRegistry().visit(
      surfaceId, [&](const ShadowTree& shadowTree) {
        status = shadowTree.commit(
            [&](const RootShadowNode& oldRootShadowNode) {
              auto* telemetry = TransactionTelemetry::threadLocalTelemetry();
              telemetry->setReleaseCoverage(
                  std::max(telemetry->getReleaseCoverage(), releaseNumber));
              auto root = std::static_pointer_cast<RootShadowNode>(
                  oldRootShadowNode.ShadowNode::clone({}));
              for (const auto& family : families) {
                auto clonedRoot = root->cloneTree(
                    *family, [&](const ShadowNode& oldShadowNode) {
                      PropsParserContext propsParserContext{
                          surfaceId, *oldShadowNode.getContextContainer()};
                      return oldShadowNode.clone(
                          {.props = oldShadowNode.getComponentDescriptor()
                                        .cloneProps(
                                            propsParserContext,
                                            oldShadowNode.getProps(),
                                            {})});
                    });
                if (clonedRoot) {
                  root = std::static_pointer_cast<RootShadowNode>(clonedRoot);
                }
              }
              return root;
            },
            {.mountSynchronously = false,
             .source = ShadowTreeCommitSource::AnimatedPropsRelease});
      });
  if (status == ShadowTree::CommitStatus::Succeeded) {
    return;
  }
  // Until a revision carries the release, its owned writes keep winning: it is
  // retried once a frame, and frames run while releases are pending.
  std::lock_guard lock(releasesMutex_);
  pendingReleaseCommits_.push_back(
      {.surfaceId = surfaceId,
       .releaseNumber = releaseNumber,
       .families = {families.begin(), families.end()}});
  if (!releaseRetryCallbackId_) {
    releaseRetryCallbackId_ =
        start([weakThis = weak_from_this()](AnimationTimestamp /*timestamp*/) {
          if (auto self = weakThis.lock()) {
            self->requestReleaseRetry();
          }
          return AnimationMutations{};
        });
  }
}

bool AnimationBackend::hasDirectWrites(SurfaceId surfaceId) {
  return animatedPropsRegistry_->hasDirectWrites(surfaceId);
}

#ifdef RN_SERIALIZABLE_STATE
folly::dynamic AnimationBackend::releasedValues(
    SurfaceId surfaceId,
    Tag tag,
    uint64_t afterReleaseCoverage,
    uint64_t releaseCoverage,
    const Props::Shared& props) {
  return animatedPropsRegistry_->releasedValues(
      surfaceId, tag, afterReleaseCoverage, releaseCoverage, props);
}

folly::dynamic AnimationBackend::restoreDirectWrites(
    SurfaceId surfaceId,
    Tag tag,
    const Props::Shared& props,
    const Props::Shared& preallocatedProps) {
  return animatedPropsRegistry_->restoreDirectWrites(
      surfaceId, tag, props, preallocatedProps);
}
#endif

void AnimationBackend::clearRegistry(SurfaceId surfaceId) {
  animatedPropsRegistry_->clear(surfaceId);
}

void AnimationBackend::clearRegistryOnSurfaceStop(SurfaceId surfaceId) {
  {
    std::lock_guard lock(releasesMutex_);
    std::erase_if(pendingReleaseCommits_, [&](const auto& releaseCommit) {
      return releaseCommit.surfaceId == surfaceId;
    });
  }
  stopReleaseRetriesIfIdle();
  animatedPropsRegistry_->clearOnSurfaceStop(surfaceId);
}

void AnimationBackend::registerJSInvoker(
    std::shared_ptr<CallInvoker> jsInvoker) {
  if (!jsInvoker_) {
    jsInvoker_ = jsInvoker;
  }
}

} // namespace facebook::react
