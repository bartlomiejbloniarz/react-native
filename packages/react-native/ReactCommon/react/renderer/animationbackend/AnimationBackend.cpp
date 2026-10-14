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

static void insertKeys(
    std::unordered_set<std::string>& keys,
    const folly::dynamic& values) {
  for (const auto& [key, _] : values.items()) {
    keys.insert(key.getString());
  }
}

// Removes the keys' values. A typed prop written as other keys too keeps them,
// as raw props, as does a typed border prop whose group has one of the keys.
static void removeKeys(
    AnimatedProps& props,
    const std::unordered_set<std::string>& keys) {
  auto rawProps =
      props.rawProps ? props.rawProps->toDynamic() : folly::dynamic::object();
  std::erase_if(props.props, [&](const auto& animatedProp) {
    auto packed = animationbackend::packAnimatedProp(animatedProp);
    if (std::none_of(keys.begin(), keys.end(), [&](const auto& key) {
          return packed.count(key) != 0 ||
              animationbackend::borderPropName(key) == animatedProp->propName;
        })) {
      return false;
    }
    for (const auto& [key, value] : packed.items()) {
      rawProps[key] = value;
    }
    return true;
  });
  for (const auto& key : keys) {
    rawProps.erase(key);
  }
  props.rawProps = std::make_unique<RawProps>(std::move(rawProps));
}

// The later of a typed and a raw value of a key wins, as in the registry.
static void mergeProps(AnimatedProps& props, AnimatedProps&& incoming) {
  // A later typed value replaces an earlier one of its prop, a whole group for
  // a border prop, before anything converts the earlier one.
  for (const auto& animatedProp : incoming.props) {
    std::erase_if(props.props, [&](const auto& existing) {
      return existing->propName == animatedProp->propName;
    });
  }
  std::unordered_set<std::string> incomingKeys;
  if (incoming.rawProps && !props.props.empty()) {
    insertKeys(incomingKeys, incoming.rawProps->toDynamic());
  }
  if (props.rawProps && !incoming.props.empty()) {
    auto rawProps = props.rawProps->toDynamic();
    for (const auto& animatedProp : incoming.props) {
      insertKeys(
          incomingKeys, animationbackend::packAnimatedProp(animatedProp));
      // A typed border prop replaces its whole group.
      for (const auto& [key, _] : rawProps.items()) {
        if (animationbackend::borderPropName(key.getString()) ==
            animatedProp->propName) {
          incomingKeys.insert(key.getString());
        }
      }
    }
  }
  if (!incomingKeys.empty()) {
    removeKeys(props, incomingKeys);
  }
  for (auto& animatedProp : incoming.props) {
    props.props.push_back(std::move(animatedProp));
  }
  if (incoming.rawProps) {
    if (props.rawProps) {
      auto merged = props.rawProps->toDynamic();
      mergeAnimatedRawProps(merged, incoming.rawProps->toDynamic());
      props.rawProps = std::make_unique<RawProps>(std::move(merged));
    } else {
      props.rawProps = std::move(incoming.rawProps);
    }
  }
}

// Combines two mutations of the same view from one frame.
static void mergeMutation(
    AnimationMutation& existing,
    AnimationMutation&& incoming) {
  mergeProps(existing.props, std::move(incoming.props));
  existing.hasLayoutUpdates |= incoming.hasLayoutUpdates;
}

namespace {

// The releases a callback run by `backend` on this thread requests.
struct CallbackReleases {
  const AnimationBackend* backend;
  std::vector<std::pair<AnimationFrontendId, AnimatedPropsRelease>> releases;
};

thread_local CallbackReleases* callbackReleases = nullptr;

} // namespace

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
        }
      };

  if (auto lockedUIManager = uiManager_.lock()) {
    lockedUIManager->addOnSurfaceStartCallback(initializeSurfaceContext);
    lockedUIManager->getShadowTreeRegistry().enumerate(
        [&](const ShadowTree& shadowTree, bool& /*stop*/) {
          animatedPropsRegistry_->initializeSurface(shadowTree.getSurfaceId());
        });
  }
}

AnimationBackend::~AnimationBackend() = default;

void AnimationBackend::unpackMutations(
    FrontendMutations& batch,
    std::unordered_map<SurfaceId, SurfaceUpdates>& surfaceUpdates,
    std::unordered_map<SurfaceId, UncommittedKeys>& uncommittedKeys,
    std::map<SurfaceId, bool>& releaseCommits) {
  auto& mutations = batch.mutations;
  for (auto& mutation : mutations.batch) {
    const auto surfaceId = mutation.family->getSurfaceId();
    const auto tag = mutation.tag;
    if (!batch.commitDirectProps && !mutation.hasLayoutUpdates) {
      insertKeys(
          uncommittedKeys[surfaceId][tag],
          animationbackend::packAnimatedProps(mutation.props));
    } else if (auto surfaceIt = uncommittedKeys.find(surfaceId);
               surfaceIt != uncommittedKeys.end() &&
               surfaceIt->second.contains(tag)) {
      folly::dynamic rawValues = mutation.props.rawProps
          ? mutation.props.rawProps->toDynamic()
          : folly::dynamic::object();
      std::erase_if(surfaceIt->second[tag], [&](const auto& key) {
        return animationbackend::writesKey(&rawValues, mutation.props, key);
      });
    }
    auto& updates = surfaceUpdates[surfaceId];
    if (auto it = updates.find(tag); it != updates.end()) {
      mergeMutation(it->second, std::move(mutation));
    } else {
      updates.emplace(tag, std::move(mutation));
    }
  }

  // Registered frontends release the values React adopts instead.
  if (batch.frontendId == kDefaultFrontend) {
    for (auto surfaceId : mutations.asyncFlushSurfaces) {
      releaseCommits[surfaceId] = true;
    }
  }
}

void AnimationBackend::applySurfaceUpdates(
    std::unordered_map<SurfaceId, SurfaceUpdates>& surfaceUpdates,
    std::unordered_map<SurfaceId, UncommittedKeys>& uncommittedKeys) {
  for (auto& [surfaceId, updates] : surfaceUpdates) {
    SurfaceUpdates layoutUpdates;
    std::unordered_map<Tag, AnimatedProps> directProps;
    for (auto& [tag, mutation] : updates) {
      if (mutation.hasLayoutUpdates) {
        layoutUpdates.emplace(tag, std::move(mutation));
      } else {
        directProps.emplace(tag, std::move(mutation.props));
      }
    }
    if (!layoutUpdates.empty()) {
      auto& surfaceUncommittedKeys = uncommittedKeys[surfaceId];
      // A platform may re-apply a view's earlier direct writes when mounting
      // it, so committed views are written directly too.
      for (auto& [tag, mutation] : layoutUpdates) {
        if (auto uiManager = uiManager_.lock()) {
          uiManager->synchronouslyUpdateViewOnUIThread(
              tag, animationbackend::packAnimatedProps(mutation.props));
        }
        if (auto it = surfaceUncommittedKeys.find(tag);
            it != surfaceUncommittedKeys.end()) {
          removeKeys(mutation.props, it->second);
        }
      }
      commitUpdates(surfaceId, layoutUpdates);
    }
    if (!directProps.empty()) {
      synchronouslyUpdateProps(directProps);
    }
  }
}

bool AnimationBackend::commitsDirectProps(AnimationFrontendId frontendId) {
  std::lock_guard lock(mutex_);
  return frontends_.at(frontendId).commitDirectProps;
}

void AnimationBackend::recordReleases(
    const std::vector<std::pair<AnimationFrontendId, AnimatedPropsRelease>>&
        releases,
    bool commitValues,
    std::map<SurfaceId, bool>& releaseCommits,
    std::vector<AnimationMutation>& restoredWrites) {
  for (const auto& [frontendId, release] : releases) {
    if (release.commitValues != commitValues) {
      continue;
    }
    for (auto surfaceId : animatedPropsRegistry_->release(
             frontendId, {release}, restoredWrites)) {
      releaseCommits.try_emplace(surfaceId, false);
    }
  }
}

void AnimationBackend::runCallbacks(
    const std::vector<FrameCallback>& callbacksToRun,
    AnimationTimestamp timestamp) {
  // Sized up front rather than grown: MSVC's std::set move isn't noexcept, so
  // growing a vector of AnimationMutations would try to copy move-only props.
  std::vector<FrontendMutations> batches(callbacksToRun.size());
  std::map<SurfaceId, bool> releaseCommits;
  for (size_t i = 0; i < callbacksToRun.size(); i++) {
    const auto& [frontendId, callback] = callbacksToRun[i];
    CallbackReleases capture{.backend = this};
    auto* outerCapture = std::exchange(callbackReleases, &capture);
    batches[i] = FrontendMutations{
        .frontendId = frontendId,
        .commitDirectProps = commitsDirectProps(frontendId),
        .mutations = callback(timestamp)};
    callbackReleases = outerCapture;
    // A callback's release ends the values written before it, unless it
    // adopts values: then it adopts the callback's values too. Restored values
    // are written in the same order.
    std::vector<AnimationMutation> restoredWrites;
    recordReleases(capture.releases, false, releaseCommits, restoredWrites);
    animatedPropsRegistry_->update(batches[i]);
    auto restoredCount = restoredWrites.size();
    recordReleases(capture.releases, true, releaseCommits, restoredWrites);
    auto& batch = batches[i].mutations.batch;
    batch.insert(
        batch.begin(),
        std::make_move_iterator(restoredWrites.begin()),
        std::make_move_iterator(restoredWrites.begin() + restoredCount));
    batch.insert(
        batch.end(),
        std::make_move_iterator(restoredWrites.begin() + restoredCount),
        std::make_move_iterator(restoredWrites.end()));
  }

  std::unordered_map<SurfaceId, SurfaceUpdates> surfaceUpdates;
  std::unordered_map<SurfaceId, UncommittedKeys> uncommittedKeys;
  for (auto& batch : batches) {
    unpackMutations(batch, surfaceUpdates, uncommittedKeys, releaseCommits);
  }
  applySurfaceUpdates(surfaceUpdates, uncommittedKeys);
  requestReleaseCommits(releaseCommits);
}

void AnimationBackend::onAnimationFrame(AnimationTimestamp timestamp) {
  std::vector<FrameCallback> callbacksToRun;
  std::vector<SurfaceId> retriedSurfaces;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [_, callback] : callbacks) {
      callbacksToRun.push_back(
          {.frontendId = kDefaultFrontend, .callback = callback});
    }
    for (AnimationFrontendId id = 1; id < frontends_.size(); id++) {
      if (startedFrontends_[id]) {
        callbacksToRun.push_back(
            {.frontendId = id, .callback = frontends_[id].onAnimationFrame});
      }
    }
    // A retry joins a release commit queued already, which then does its
    // work; it never waits outside the queue.
    for (auto [surfaceId, endSync] : cancelledReleaseCommits_) {
      auto [it, isNew] = queuedReleaseCommits_.try_emplace(surfaceId, endSync);
      it->second = it->second || endSync;
      if (isNew) {
        retriedSurfaces.push_back(surfaceId);
      }
    }
    cancelledReleaseCommits_.clear();
    // Cancelled release commits keep frames coming until they are retried.
    pauseFramesIfIdle();
  }
  for (auto surfaceId : retriedSurfaces) {
    postReleaseCommit(surfaceId);
  }
  runCallbacks(callbacksToRun, timestamp);
}

AnimationFrontendId AnimationBackend::registerFrontend(
    AnimationFrontend frontend) {
  std::lock_guard lock(mutex_);
  frontends_.push_back(std::move(frontend));
  startedFrontends_.push_back(false);
  return static_cast<AnimationFrontendId>(frontends_.size() - 1);
}

bool AnimationBackend::hasFrameCallbacks() const {
  return !callbacks.empty() ||
      std::find(startedFrontends_.begin(), startedFrontends_.end(), true) !=
      startedFrontends_.end();
}

void AnimationBackend::resumeFrames() {
  if (!isRenderCallbackStarted_) {
    animationChoreographer_->resume();
    isRenderCallbackStarted_ = true;
  }
}

void AnimationBackend::pauseFramesIfIdle() {
  if (isRenderCallbackStarted_ && !hasFrameCallbacks() &&
      cancelledReleaseCommits_.empty()) {
    animationChoreographer_->pause();
    isRenderCallbackStarted_ = false;
  }
}

void AnimationBackend::start(AnimationFrontendId frontendId) {
  std::lock_guard lock(mutex_);
  if (startedFrontends_.at(frontendId) ||
      !frontends_.at(frontendId).onAnimationFrame) {
    return;
  }
  startedFrontends_[frontendId] = true;
  resumeFrames();
}

void AnimationBackend::stop(AnimationFrontendId frontendId) {
  std::lock_guard lock(mutex_);
  if (!startedFrontends_.at(frontendId)) {
    return;
  }
  startedFrontends_[frontendId] = false;
  pauseFramesIfIdle();
}

CallbackId AnimationBackend::start(const Callback& callback) {
  std::lock_guard lock(mutex_);
  auto callbackId = nextCallbackId_++;
  callbacks.push_back({.callbackId = callbackId, .callback = callback});
  resumeFrames();
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
  pauseFramesIfIdle();
}

void AnimationBackend::trigger() {
  onAnimationFrame(std::chrono::steady_clock::now().time_since_epoch());
}

void AnimationBackend::pushAnimationMutations(const Callback& callback) {
  pushAnimationMutations(kDefaultFrontend, callback);
}

void AnimationBackend::pushAnimationMutations(
    AnimationFrontendId frontendId,
    const Callback& callback) {
  runCallbacks(
      {{.frontendId = frontendId, .callback = callback}},
      animationChoreographer_->now());
}

void AnimationBackend::release(
    AnimationFrontendId frontendId,
    std::vector<AnimatedPropsRelease> releases) {
  if (callbackReleases != nullptr && callbackReleases->backend == this) {
    for (auto& release : releases) {
      callbackReleases->releases.emplace_back(frontendId, std::move(release));
    }
    return;
  }
  std::map<SurfaceId, bool> releaseCommits;
  std::vector<AnimationMutation> restoredWrites;
  for (auto surfaceId :
       animatedPropsRegistry_->release(frontendId, releases, restoredWrites)) {
    releaseCommits.emplace(surfaceId, false);
  }
  for (auto& restoredWrite : restoredWrites) {
    std::unordered_map<Tag, AnimatedProps> updates;
    updates.emplace(restoredWrite.tag, std::move(restoredWrite.props));
    synchronouslyUpdateProps(updates);
  }
  requestReleaseCommits(releaseCommits);
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
  if (ReactNativeFeatureFlags::optimizedAnimatedPropUpdates()) {
    if (auto uiManager = uiManager_.lock()) {
      uiManager->synchronouslyUpdateAnimatedProps(updates);
    }
    return;
  }
  for (auto& [tag, animatedProps] : updates) {
    // TODO: We shouldn't repack it into dynamic, but for that a rewrite
    // of synchronouslyUpdateViewOnUIThread is needed
    auto dyn = animationbackend::packAnimatedProps(animatedProps);
    if (auto uiManager = uiManager_.lock()) {
      uiManager->synchronouslyUpdateViewOnUIThread(tag, dyn);
    }
  }
}

void AnimationBackend::requestAsyncFlushForSurfaces(
    const std::set<SurfaceId>& surfaces) {
  std::map<SurfaceId, bool> releaseCommits;
  for (auto surfaceId : surfaces) {
    releaseCommits[surfaceId] = true;
  }
  requestReleaseCommits(releaseCommits);
}

void AnimationBackend::requestReleaseCommits(
    const std::map<SurfaceId, bool>& releaseCommits) {
  react_native_assert(
      jsInvoker_ != nullptr ||
      releaseCommits.empty() && "jsInvoker_ was not provided");
  for (auto [surfaceId, endSync] : releaseCommits) {
    {
      std::lock_guard lock(mutex_);
      auto [it, isNew] = queuedReleaseCommits_.try_emplace(surfaceId, endSync);
      it->second = it->second || endSync;
      if (!isNew) {
        continue;
      }
    }
    postReleaseCommit(surfaceId);
  }
}

void AnimationBackend::postReleaseCommit(SurfaceId surfaceId) {
  jsInvoker_->invokeAsync([weakThis = weak_from_this(), surfaceId]() {
    if (auto self = weakThis.lock()) {
      self->commitReleases(surfaceId);
    }
  });
}

void AnimationBackend::commitReleases(SurfaceId surfaceId) {
  bool endSync = false;
  {
    std::lock_guard lock(mutex_);
    auto it = queuedReleaseCommits_.find(surfaceId);
    if (it == queuedReleaseCommits_.end()) {
      return;
    }
    endSync = it->second;
    queuedReleaseCommits_.erase(it);
    // This commit also does the work of an earlier cancelled one.
    if (auto cancelledIt = cancelledReleaseCommits_.find(surfaceId);
        cancelledIt != cancelledReleaseCommits_.end()) {
      endSync = endSync || cancelledIt->second;
      cancelledReleaseCommits_.erase(cancelledIt);
    }
  }
  auto uiManager = uiManager_.lock();
  if (!uiManager) {
    return;
  }
  auto releaseCommit = animatedPropsRegistry_->releaseCommit(surfaceId);
  if (!endSync && releaseCommit.families.empty()) {
    return;
  }
  std::unordered_set<std::shared_ptr<const ShadowNodeFamily>> families(
      releaseCommit.families.begin(), releaseCommit.families.end());
  auto& adoptedValues = releaseCommit.adoptedValues;
  std::optional<ShadowTree::CommitStatus> status;
  uiManager->getShadowTreeRegistry().visit(
      surfaceId, [&](const ShadowTree& shadowTree) {
        status = shadowTree.commit(
            [&](const RootShadowNode& oldRootShadowNode) {
              // A released view gets fresh props, so the release revision
              // has an Update for it.
              auto newRootShadowNode = oldRootShadowNode.cloneMultiple(
                  families,
                  [&](const ShadowNode& shadowNode,
                      const ShadowNodeFragment& fragment) {
                    auto newProps = ShadowNodeFragment::propsPlaceholder();
                    if (families.contains(shadowNode.getFamilyShared())) {
                      auto it = adoptedValues.find(shadowNode.getTag());
                      newProps = shadowNode.getComponentDescriptor().cloneProps(
                          PropsParserContext{
                              surfaceId, *shadowNode.getContextContainer()},
                          shadowNode.getProps(),
                          RawProps(
                              it != adoptedValues.end()
                                  ? it->second
                                  : folly::dynamic::object()));
                    }
                    return shadowNode.clone(
                        {.props = newProps,
                         .children = fragment.children,
                         .state = shadowNode.getState()});
                  });
              return std::static_pointer_cast<RootShadowNode>(
                  newRootShadowNode ? newRootShadowNode
                                    : oldRootShadowNode.ShadowNode::clone({}));
            },
            {.source = ShadowTreeCommitSource::AnimationEndSync});
      });
  if (status == ShadowTree::CommitStatus::Succeeded) {
    animatedPropsRegistry_->didCommitReleases(surfaceId, releaseCommit.release);
    // To clear the registry, the updates neeed to be propagated to React with
    // RSNRU. Without updateRuntimeShadowNodeReferencesOnCommitThread this won't
    // happen if we do any commits on the main thread, since the
    // runtimeShadowNodeReference_ is not propagated to nodes cloned outside of
    // the JS thread. So when the flag is disabled we keep the updates in the
    // registry and we will reapply them in a commit hook triggered by a
    // rerender.
    if (ReactNativeFeatureFlags::
            updateRuntimeShadowNodeReferencesOnCommitThread()) {
      animatedPropsRegistry_->stopStamping(surfaceId, releaseCommit.release);
      if (endSync) {
        animatedPropsRegistry_->clear(surfaceId);
      }
    }
  } else if (status.has_value() && !releaseCommit.families.empty()) {
    std::lock_guard lock(mutex_);
    auto& cancelledEndSync = cancelledReleaseCommits_[surfaceId];
    cancelledEndSync = cancelledEndSync || endSync;
    resumeFrames();
  }
}

void AnimationBackend::clearRegistry(SurfaceId surfaceId) {
  animatedPropsRegistry_->clear(surfaceId);
}

void AnimationBackend::clearRegistryOnSurfaceStop(SurfaceId surfaceId) {
  {
    std::lock_guard lock(mutex_);
    queuedReleaseCommits_.erase(surfaceId);
    cancelledReleaseCommits_.erase(surfaceId);
  }
  animatedPropsRegistry_->clearOnSurfaceStop(surfaceId);
}

void AnimationBackend::registerJSInvoker(
    std::shared_ptr<CallInvoker> jsInvoker) {
  if (!jsInvoker_) {
    jsInvoker_ = jsInvoker;
  }
}

} // namespace facebook::react
