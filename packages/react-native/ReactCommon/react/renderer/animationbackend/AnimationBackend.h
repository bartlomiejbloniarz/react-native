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
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/uimanager/UIManagerAnimationBackend.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>
#include "AnimatedProps.h"
#include "AnimationChoreographer.h"
#include "AnimationMutation.h"

namespace facebook::react {

class AnimationBackend;
class AnimationBackendCommitHook;
class AnimatedPropsRegistry;

using Callback = std::function<AnimationMutations(AnimationTimestamp)>;

struct CallbackWithId {
  CallbackId callbackId;
  Callback callback;
};

class AnimationBackend : public UIManagerAnimationBackend, public std::enable_shared_from_this<AnimationBackend> {
 public:
  using ResumeCallback = std::function<void()>;
  using PauseCallback = std::function<void()>;

  AnimationBackend(
      std::shared_ptr<AnimationChoreographer> animationChoreographer,
      std::shared_ptr<UIManager> uiManager);
  ~AnimationBackend() override;
  void synchronouslyUpdateProps(const std::unordered_map<Tag, AnimatedProps> &updates);
  void requestAsyncFlushForSurfaces(const std::set<SurfaceId> &surfaces);
  void clearRegistry(SurfaceId surfaceId) override;
  void clearRegistryOnSurfaceStop(SurfaceId surfaceId) override;
  void registerJSInvoker(std::shared_ptr<CallInvoker> jsInvoker) override;
  std::vector<folly::dynamic> newerDirectWrites(SurfaceId surfaceId, const std::vector<StampedView> &stampedViews)
      override;
  bool hasDirectWrites(SurfaceId surfaceId) override;
#ifdef RN_SERIALIZABLE_STATE
  folly::dynamic restoreDirectWrites(
      SurfaceId surfaceId,
      Tag tag,
      const Props::Shared &props,
      const Props::Shared &preallocatedProps) override;
  folly::dynamic releasedValues(
      SurfaceId surfaceId,
      Tag tag,
      uint64_t afterReleaseCoverage,
      uint64_t releaseCoverage,
      const Props::Shared &props) override;
#endif
  void own(
      const std::shared_ptr<const ShadowNodeFamily> &family,
      Tag propsNodeTag,
      const std::vector<std::string> &keys) override;
  void release(Tag propsNodeTag) override;

  void onAnimationFrame(AnimationTimestamp timestamp) override;
  void trigger() override;
  void pushAnimationMutations(const Callback &callback) override;
  CallbackId start(const Callback &callback) override;
  void stop(CallbackId callbackId) override;

 private:
  using SurfaceUpdates = std::unordered_map<Tag, AnimationMutation>;

  void commitUpdates(SurfaceId surfaceId, SurfaceUpdates &surfaceUpdates);
  void unpackMutations(
      AnimationMutations &mutations,
      std::unordered_map<SurfaceId, SurfaceUpdates> &surfaceUpdates,
      std::set<SurfaceId> &asyncFlushSurfaces);
  void applySurfaceUpdates(
      std::unordered_map<SurfaceId, SurfaceUpdates> &surfaceUpdates,
      const std::set<SurfaceId> &asyncFlushSurfaces);
  void applyMutations(std::vector<AnimationMutations> batches);
  struct ReleaseCommit {
    SurfaceId surfaceId;
    uint64_t releaseNumber;
    std::vector<std::weak_ptr<const ShadowNodeFamily>> families;
  };

  // Commits the releases requested since the last frame on the JS thread.
  void requestReleaseCommit();
  // Releases the connections with one commit per surface whose released views
  // were written, so React's values are mounted again.
  void commitReleases(const std::vector<uint64_t> &connections);
  // Commits the release, or keeps it pending when the commit is not accepted.
  void commitRelease(const ReleaseCommit &releaseCommit);
  void commitPendingReleases();
  // Requests one JS task committing the pending releases, unless one is
  // requested already.
  void requestReleaseRetry();
  void stopReleaseRetriesIfIdle();
  std::vector<CallbackWithId> callbacks;
  std::shared_ptr<AnimatedPropsRegistry> animatedPropsRegistry_;
  std::shared_ptr<AnimationChoreographer> animationChoreographer_;
  std::unique_ptr<AnimationBackendCommitHook> commitHook_;
  std::weak_ptr<UIManager> uiManager_;
  std::shared_ptr<CallInvoker> jsInvoker_;
  std::mutex releasesMutex_;
  std::vector<uint64_t> releasedConnections_;
  std::vector<ReleaseCommit> pendingReleaseCommits_;
  std::optional<CallbackId> releaseRetryCallbackId_;
  bool isReleaseRetryRequested_{false};
  bool isRenderCallbackStarted_{false};
  CallbackId nextCallbackId_{0};
  std::mutex mutex_;
};
} // namespace facebook::react
