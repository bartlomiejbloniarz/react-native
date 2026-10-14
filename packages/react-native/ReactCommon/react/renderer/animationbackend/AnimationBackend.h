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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
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

struct FrameCallback {
  AnimationFrontendId frontendId;
  Callback callback;
};

struct FrontendMutations;

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

  void onAnimationFrame(AnimationTimestamp timestamp) override;
  void trigger() override;
  AnimationFrontendId registerFrontend(AnimationFrontend frontend) override;
  void start(AnimationFrontendId frontendId) override;
  void stop(AnimationFrontendId frontendId) override;
  void pushAnimationMutations(AnimationFrontendId frontendId, const Callback &callback) override;
  // A release requested while the backend runs a frontend's callback ends the
  // values written before the callback, unless it commits the values: then it
  // also commits the callback's values.
  void release(AnimationFrontendId frontendId, std::vector<AnimatedPropsRelease> releases) override;
  void pushAnimationMutations(const Callback &callback) override;
  CallbackId start(const Callback &callback) override;
  void stop(CallbackId callbackId) override;

 private:
  static constexpr AnimationFrontendId kDefaultFrontend{0};
  using SurfaceUpdates = std::unordered_map<Tag, AnimationMutation>;
  // Per view, the keys whose latest value in a frame stays out of commits.
  using UncommittedKeys = std::unordered_map<Tag, std::unordered_set<std::string>>;

  void commitUpdates(SurfaceId surfaceId, SurfaceUpdates &surfaceUpdates);
  void unpackMutations(
      FrontendMutations &batch,
      std::unordered_map<SurfaceId, SurfaceUpdates> &surfaceUpdates,
      std::unordered_map<SurfaceId, UncommittedKeys> &uncommittedKeys,
      std::map<SurfaceId, bool> &releaseCommits);
  void applySurfaceUpdates(
      std::unordered_map<SurfaceId, SurfaceUpdates> &surfaceUpdates,
      std::unordered_map<SurfaceId, UncommittedKeys> &uncommittedKeys);
  // Runs the callbacks and records each one's mutations and releases in
  // order, then writes the mutations to views.
  void runCallbacks(const std::vector<FrameCallback> &callbacksToRun, AnimationTimestamp timestamp);
  void recordReleases(
      const std::vector<std::pair<AnimationFrontendId, AnimatedPropsRelease>> &releases,
      bool commitValues,
      std::map<SurfaceId, bool> &releaseCommits,
      std::vector<AnimationMutation> &restoredWrites);
  // Queues one release commit per surface; `true` when it adopts values (an
  // end sync).
  void requestReleaseCommits(const std::map<SurfaceId, bool> &releaseCommits);
  void postReleaseCommit(SurfaceId surfaceId);
  void commitReleases(SurfaceId surfaceId);
  bool commitsDirectProps(AnimationFrontendId frontendId);
  bool hasFrameCallbacks() const;
  void resumeFrames();
  void pauseFramesIfIdle();
  friend class AnimationBackendFrontendTest;
  std::vector<CallbackWithId> callbacks;
  std::vector<AnimationFrontend> frontends_{AnimationFrontend{}};
  std::vector<bool> startedFrontends_{false};
  std::map<SurfaceId, bool> queuedReleaseCommits_;
  std::map<SurfaceId, bool> cancelledReleaseCommits_;
  std::shared_ptr<AnimatedPropsRegistry> animatedPropsRegistry_;
  std::shared_ptr<AnimationChoreographer> animationChoreographer_;
  std::unique_ptr<AnimationBackendCommitHook> commitHook_;
  std::weak_ptr<UIManager> uiManager_;
  std::shared_ptr<CallInvoker> jsInvoker_;
  bool isRenderCallbackStarted_{false};
  CallbackId nextCallbackId_{0};
  std::mutex mutex_;
};
} // namespace facebook::react
