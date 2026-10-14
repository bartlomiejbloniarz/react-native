/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/featureflags/ReactNativeFeatureFlagsDefaults.h>
#include <react/renderer/animated/NativeAnimatedNodesManager.h>
#include <react/renderer/animationbackend/AnimatedPropsBuilder.h>
#include <react/renderer/animationbackend/AnimatedPropsRegistry.h>
#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/animationbackend/AnimationBackend.h>
#include <react/renderer/components/root/RootShadowNode.h>
#include <react/renderer/components/view/ViewShadowNode.h>
#include <react/renderer/element/ComponentBuilder.h>
#include <react/renderer/element/Element.h>
#include <react/renderer/element/testUtils.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/uimanager/UIManagerCommitHook.h>
#include <react/renderer/uimanager/UIManagerDelegate.h>
#include <react/utils/ContextContainer.h>
#include <limits>

namespace facebook::react {

namespace {

class TestAnimationChoreographer : public AnimationChoreographer {
 public:
  bool isResumed{false};

  void resume() override {
    isResumed = true;
  }
  void pause() override {
    isResumed = false;
  }
};

class RecordingUIManagerDelegate : public UIManagerDelegate {
 public:
  std::unordered_map<Tag, folly::dynamic> synchronousUpdates;
  std::function<void()> onSynchronousUpdate;

  void uiManagerShouldSynchronouslyUpdateViewOnUIThread(
      Tag tag,
      const folly::dynamic& props) override {
    synchronousUpdates.try_emplace(tag, folly::dynamic::object())
        .first->second.update(props);
    if (onSynchronousUpdate) {
      std::exchange(onSynchronousUpdate, nullptr)();
    }
  }

  void uiManagerShouldSynchronouslyUpdateAnimatedProps(
      const std::unordered_map<Tag, AnimatedProps>& updates) override {
    for (const auto& [tag, props] : updates) {
      uiManagerShouldSynchronouslyUpdateViewOnUIThread(
          tag, animationbackend::packAnimatedProps(props));
    }
  }
  void uiManagerDidFinishTransaction(
      std::shared_ptr<const MountingCoordinator> /*mountingCoordinator*/,
      bool /*mountSynchronously*/) override {}
  void uiManagerDidCreateShadowNode(const ShadowNode& /*shadowNode*/) override {
  }
  void uiManagerDidDispatchCommand(
      const std::shared_ptr<const ShadowNode>& /*shadowNode*/,
      const std::string& /*commandName*/,
      const folly::dynamic& /*args*/) override {}
  void uiManagerDidSendAccessibilityEvent(
      const std::shared_ptr<const ShadowNode>& /*shadowNode*/,
      const std::string& /*eventType*/) override {}
  void uiManagerDidSetIsJSResponder(
      const std::shared_ptr<const ShadowNode>& /*shadowNode*/,
      bool /*isJSResponder*/,
      bool /*blockNativeResponder*/) override {}
  void uiManagerDidUpdateShadowTree(
      const std::unordered_map<Tag, folly::dynamic>& /*tagToProps*/) override {}
  void uiManagerShouldAddEventListener(
      std::shared_ptr<const EventListener> /*listener*/) override {}
  void uiManagerShouldRemoveEventListener(
      const std::shared_ptr<const EventListener>& /*listener*/) override {}
  void uiManagerDidStartSurface(const ShadowTree& /*shadowTree*/) override {}
  void uiManagerDidFinishReactCommit(
      const ShadowTree& /*shadowTree*/) override {}
  void uiManagerShouldAddOnSurfaceStartCallback(
      OnSurfaceStartCallback&& /*callback*/) override {}
  void uiManagerDidCaptureViewSnapshot(Tag /*tag*/, SurfaceId /*surfaceId*/)
      override {}
  void uiManagerDidSetViewSnapshot(
      Tag /*sourceTag*/,
      Tag /*targetTag*/,
      SurfaceId /*surfaceId*/) override {}
  void uiManagerDidClearPendingSnapshots() override {}
};

// Runs JS tasks immediately, or queues them while deferring.
class TestCallInvoker : public CallInvoker {
 public:
  bool defers{false};
  std::vector<std::function<void()>> tasks;

  void invokeAsync(CallFunc&& /*func*/) noexcept override {}
  void invokeAsync(std::function<void()>&& func) noexcept override {
    if (defers) {
      tasks.push_back(std::move(func));
    } else {
      func();
    }
  }
  void invokeSync(CallFunc&& /*func*/) override {}

  void runTasks() {
    for (auto& task : std::exchange(tasks, {})) {
      task();
    }
  }
};

class TestFeatureFlags : public ReactNativeFeatureFlagsDefaults {
 public:
  bool batchesDirectWrites{false};
  bool updatesRuntimeReferences{false};
  bool sharesAnimatedBackend{false};

  bool optimizedAnimatedPropUpdates() override {
    return batchesDirectWrites;
  }
  bool updateRuntimeShadowNodeReferencesOnCommitThread() override {
    return updatesRuntimeReferences;
  }
  bool useSharedAnimatedBackend() override {
    return sharesAnimatedBackend;
  }
};

class CancellingCommitHook : public UIManagerCommitHook {
 public:
  bool cancelsEndSyncs{false};
  // Runs once in the next end sync, after the animation backend's hook.
  std::function<void()> duringEndSync;

  void commitHookWasRegistered(
      const UIManager& /*uiManager*/) noexcept override {}
  void commitHookWasUnregistered(
      const UIManager& /*uiManager*/) noexcept override {}
  RootShadowNode::Unshared shadowTreeWillCommit(
      const ShadowTree& /*shadowTree*/,
      const RootShadowNode::Shared& /*oldRootShadowNode*/,
      const RootShadowNode::Unshared& newRootShadowNode,
      const ShadowTreeCommitOptions& commitOptions) noexcept override {
    if (duringEndSync &&
        commitOptions.source == ShadowTreeCommitSource::AnimationEndSync) {
      std::exchange(duringEndSync, nullptr)();
    }
    return cancelsEndSyncs &&
            commitOptions.source == ShadowTreeCommitSource::AnimationEndSync
        ? nullptr
        : newRootShadowNode;
  }
};

constexpr SurfaceId kSurface = 1;
constexpr Tag kView = 10;

} // namespace

class AnimationBackendTest : public ::testing::Test {
 protected:
  std::shared_ptr<ContextContainer> contextContainer =
      std::make_shared<ContextContainer>();
  ComponentBuilder builder = simpleComponentBuilder(contextContainer);
  std::shared_ptr<ViewShadowNode> node =
      builder.build(Element<ViewShadowNode>().surfaceId(1).tag(10));
  RecordingUIManagerDelegate delegate;
  std::shared_ptr<UIManager> uiManager = std::make_shared<UIManager>(
      [](std::function<void(jsi::Runtime&)>&& /*callback*/) {},
      contextContainer);
  std::shared_ptr<AnimationBackend> backend;

  void SetUp() override {
    uiManager->setDelegate(&delegate);
    backend = std::make_shared<AnimationBackend>(
        std::make_shared<TestAnimationChoreographer>(), uiManager);
  }

  void TearDown() override {
    backend.reset();
    uiManager->setDelegate(nullptr);
  }
};

TEST_F(AnimationBackendTest, layoutViewWritesNonLayoutPropsSynchronously) {
  backend->pushAnimationMutations([this](AnimationTimestamp /*timestamp*/) {
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setWidth(yoga::StyleSizeLength::points(100));
    propsBuilder.setOpacity(0.5);
    AnimationMutations mutations;
    mutations.batch.push_back(
        AnimationMutation{
            .tag = 10,
            .family = node->getFamilyShared(),
            .props = propsBuilder.get(),
            .hasLayoutUpdates = true});
    return mutations;
  });

  ASSERT_EQ(delegate.synchronousUpdates.size(), 1u);
  folly::dynamic expected = folly::dynamic::object("opacity", 0.5);
  EXPECT_EQ(delegate.synchronousUpdates.at(10), expected);
}

// A view on a running surface whose React opacity is 0.8.
class AnimationBackendFrontendTest : public ::testing::Test {
 protected:
  std::shared_ptr<ContextContainer> contextContainer =
      std::make_shared<ContextContainer>();
  ComponentBuilder builder = simpleComponentBuilder(contextContainer);
  RecordingUIManagerDelegate delegate;
  CancellingCommitHook cancellingCommitHook;
  std::shared_ptr<UIManager> uiManager = std::make_shared<UIManager>(
      [](std::function<void(jsi::Runtime&)>&& /*callback*/) {},
      contextContainer);
  std::shared_ptr<ViewShadowNode> view;
  ShadowTree* shadowTree{nullptr};
  std::shared_ptr<AnimationBackend> backend;
  std::shared_ptr<TestAnimationChoreographer> choreographer =
      std::make_shared<TestAnimationChoreographer>();
  std::shared_ptr<TestCallInvoker> jsInvoker =
      std::make_shared<TestCallInvoker>();
  std::shared_ptr<NativeAnimatedNodesManager> nodesManager;
  double frameTime{1000};
  AnimationFrontendId animated{};
  AnimationFrontendId reanimated{};

  void SetUp() override {
    uiManager->setDelegate(&delegate);
    view = builder.build(
        Element<ViewShadowNode>().tag(kView).surfaceId(kSurface).props([] {
          auto props = std::make_shared<ViewProps>();
          props->opacity = 0.8;
          return props;
        }));
    auto tree = std::make_unique<ShadowTree>(
        kSurface,
        LayoutConstraints{},
        LayoutContext{},
        *uiManager,
        *contextContainer);
    shadowTree = tree.get();
    tree->commit(
        [&](const RootShadowNode& oldRootShadowNode) {
          return std::static_pointer_cast<RootShadowNode>(
              oldRootShadowNode.ShadowNode::clone(
                  {.children = std::make_shared<
                       std::vector<std::shared_ptr<const ShadowNode>>>(
                       std::vector<std::shared_ptr<const ShadowNode>>{view})}));
        },
        {});
    uiManager->startSurface(
        std::move(tree), "test", folly::dynamic::object, DisplayMode::Visible);
    backend = std::make_shared<AnimationBackend>(choreographer, uiManager);
    backend->registerJSInvoker(jsInvoker);
    uiManager->registerCommitHook(cancellingCommitHook);
    animated = backend->registerFrontend({.commitDirectProps = false});
    reanimated = backend->registerFrontend({.commitDirectProps = true});
  }

  void TearDown() override {
    nodesManager.reset();
    uiManager->unregisterCommitHook(cancellingCommitHook);
    uiManager->unstable_setAnimationBackend(nullptr);
    backend.reset();
    uiManager->stopSurface(kSurface);
    uiManager->setDelegate(nullptr);
    ReactNativeFeatureFlags::dangerouslyReset();
  }

  AnimationMutations mutations(
      const folly::dynamic& rawProps,
      bool hasLayoutUpdates = false) const {
    AnimationMutations mutations;
    mutations.batch.push_back(
        AnimationMutation{
            .tag = kView,
            .family = view->getFamilyShared(),
            .props =
                {.props = {}, .rawProps = std::make_unique<RawProps>(rawProps)},
            .hasLayoutUpdates = hasLayoutUpdates});
    return mutations;
  }

  void write(AnimationFrontendId frontendId, const folly::dynamic& rawProps) {
    backend->pushAnimationMutations(
        frontendId,
        [&](AnimationTimestamp /*timestamp*/) { return mutations(rawProps); });
  }

  void release(
      AnimationFrontendId frontendId,
      bool commitValues,
      std::string key = "opacity",
      folly::dynamic restoredValues = folly::dynamic::object()) {
    backend->release(
        frontendId,
        {{.family = view->getFamilyShared(),
          .keys = {std::move(key)},
          .commitValues = commitValues,
          .restoredValues = std::move(restoredValues)}});
  }

  static void overrideFlags(
      bool batchesDirectWrites,
      bool updatesRuntimeReferences,
      bool sharesAnimatedBackend = false) {
    ReactNativeFeatureFlags::dangerouslyReset();
    auto flags = std::make_unique<TestFeatureFlags>();
    flags->batchesDirectWrites = batchesDirectWrites;
    flags->updatesRuntimeReferences = updatesRuntimeReferences;
    flags->sharesAnimatedBackend = sharesAnimatedBackend;
    ReactNativeFeatureFlags::override(std::move(flags));
  }

  // Registers a frontend and starts it. Started frontends run in the order
  // they were registered.
  AnimationFrontendId startFrontend(
      bool commitDirectProps,
      Callback onAnimationFrame) {
    auto frontendId = backend->registerFrontend(
        {.commitDirectProps = commitDirectProps,
         .onAnimationFrame = std::move(onAnimationFrame)});
    backend->start(frontendId);
    return frontendId;
  }

  // A stamped frontend writes typed opacity 0.3 and an uncommitted one raw
  // opacity 0.5 in one frame, in this order unless `rawFirst`.
  void writeTypedAndRawOpacity(bool rawFirst) {
    Callback typed = [this](AnimationTimestamp /*timestamp*/) {
      AnimatedPropsBuilder propsBuilder;
      propsBuilder.setOpacity(0.3);
      AnimationMutations frame;
      frame.batch.push_back(
          AnimationMutation{
              .tag = kView,
              .family = view->getFamilyShared(),
              .props = propsBuilder.get()});
      return frame;
    };
    Callback raw = [this](AnimationTimestamp /*timestamp*/) {
      return mutations(opacity(0.5));
    };
    if (rawFirst) {
      startFrontend(false, raw);
      startFrontend(true, typed);
    } else {
      startFrontend(true, typed);
      startFrontend(false, raw);
    }
    frame();
  }

  AnimationMutations typedMutations(AnimatedProps props) const {
    AnimationMutations frame;
    frame.batch.push_back(
        AnimationMutation{
            .tag = kView,
            .family = view->getFamilyShared(),
            .props = std::move(props)});
    return frame;
  }

  static AnimatedProps typedOpacity(Float value) {
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setOpacity(value);
    return propsBuilder.get();
  }

  // Top left 3 points, top right 50%.
  static AnimatedProps typedBorderRadii() {
    CascadedBorderRadii borderRadii;
    borderRadii.topLeft = ValueUnit(3, UnitType::Point);
    borderRadii.topRight = ValueUnit(50, UnitType::Percent);
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setBorderRadii(borderRadii);
    return propsBuilder.get();
  }

  template <typename Value>
  void writeTyped(PropName propName, const Value& value) {
    backend->pushAnimationMutations(
        reanimated, [&](AnimationTimestamp /*timestamp*/) {
          AnimatedProps props;
          props.props.push_back(
              std::make_unique<AnimatedProp<Value>>(propName, value));
          return typedMutations(std::move(props));
        });
  }

  const ViewProps& viewProps() const {
    return static_cast<const ViewProps&>(*treeProps());
  }

  const CascadedBorderRadii& treeBorderRadii() const {
    return static_cast<const ViewProps&>(*treeProps()).borderRadii;
  }

  // Two committed writers of opacity in separate frames: typed 0.3 from
  // Reanimated and raw 0.5 from an unregistered producer.
  void writeCommittedOpacities(bool rawFirst, bool isCommittedBetween) {
    auto writeTyped = [&]() {
      backend->pushAnimationMutations(
          reanimated, [&](AnimationTimestamp /*timestamp*/) {
            return typedMutations(typedOpacity(0.3));
          });
    };
    auto writeRaw = [&]() {
      backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
        return mutations(opacity(0.5));
      });
    };
    rawFirst ? writeRaw() : writeTyped();
    if (isCommittedBetween) {
      reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));
    }
    rawFirst ? writeTyped() : writeRaw();
    reactCommit(folly::dynamic::object("backgroundColor", 0xff00ff00));
  }

  void expectLaterRawValueWins() {
    writeTypedAndRawOpacity(false);

    EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], 0.5);
    EXPECT_EQ(pull().overlaid.at(kView), opacity(0.5));
    EXPECT_EQ(stampedSnapshot(), nullptr);
  }

  void expectLaterTypedValueWins() {
    writeTypedAndRawOpacity(true);

    EXPECT_FLOAT_EQ(
        delegate.synchronousUpdates.at(kView)["opacity"].asDouble(), 0.3);
    EXPECT_TRUE(pull().overlaid.empty());
    EXPECT_TRUE(stampedSnapshot()->propNames.contains(OPACITY));
  }

  void reactCommit(const folly::dynamic& rawProps) {
    shadowTree->commit(
        [&](const RootShadowNode& oldRootShadowNode) {
          return std::static_pointer_cast<RootShadowNode>(
              oldRootShadowNode.cloneTree(
                  view->getFamily(), [&](const ShadowNode& oldShadowNode) {
                    PropsParserContext context{kSurface, *contextContainer};
                    return oldShadowNode.clone(
                        {.props =
                             oldShadowNode.getComponentDescriptor().cloneProps(
                                 context,
                                 oldShadowNode.getProps(),
                                 RawProps(rawProps))});
                  }));
        },
        {.source = ShadowTreeCommitSource::React});
  }

  // A React commit through UIManager, which clears the registry after it.
  void completeSurface(const folly::dynamic& rawProps) {
    PropsParserContext context{kSurface, *contextContainer};
    auto child = view->clone(
        {.props = view->getComponentDescriptor().cloneProps(
             context, view->getProps(), RawProps(rawProps))});
    uiManager->completeSurface(
        kSurface,
        std::make_shared<std::vector<std::shared_ptr<const ShadowNode>>>(
            std::vector<std::shared_ptr<const ShadowNode>>{child}),
        {.source = ShadowTreeCommitSource::React});
  }

  Props::Shared treeProps() const {
    const auto& root = *shadowTree->getCurrentRevision().rootShadowNode;
    auto ancestors = view->getFamily().getAncestors(root);
    const auto& [parent, index] = ancestors.back();
    return parent.get().getChildren().at(index)->getProps();
  }

  Float treeOpacity() const {
    return static_cast<const ViewProps&>(*treeProps()).opacity;
  }

  static folly::dynamic opacity(double value) {
    return folly::dynamic::object("opacity", value);
  }

  void frame() {
    backend->onAnimationFrame(AnimationTimestamp{frameTime += 17});
  }

  // A C++ Animated props node `base + 2` writing `key` of the view from value
  // node `base`.
  void connectNode(Tag base, const std::string& key, double value) {
    if (!nodesManager) {
      nodesManager = NativeAnimatedNodesManager::create(backend);
    }
    auto family =
        std::const_pointer_cast<ShadowNodeFamily>(view->getFamilyShared());
    nodesManager->scheduleOnUI(
        [manager = nodesManager, family, base, key, value]() {
          manager->createAnimatedNode(
              base,
              folly::dynamic::object("type", "value")("value", value)(
                  "offset", 0));
          manager->createAnimatedNode(
              base + 1,
              folly::dynamic::object("type", "style")(
                  "style", folly::dynamic::object(key, base)));
          manager->createAnimatedNode(
              base + 2,
              folly::dynamic::object("type", "props")(
                  "props", folly::dynamic::object("style", base + 1)));
          manager->connectAnimatedNodes(base, base + 1);
          manager->connectAnimatedNodes(base + 1, base + 2);
          manager->connectAnimatedNodeToView(base + 2, kView);
          manager->connectAnimatedNodeToShadowNodeFamily(base + 2, family);
        });
  }

  void disconnectNode(Tag base) {
    nodesManager->scheduleOnUI([manager = nodesManager, base]() {
      manager->disconnectAnimatedNodeFromView(base + 2, kView);
    });
  }

  std::shared_ptr<const PropsSnapshot> stampedSnapshot() const {
    auto snapshots = backend->animatedPropsRegistry_->getMap(kSurface).second;
    auto it = snapshots.find(kView);
    return it != snapshots.end() ? it->second : nullptr;
  }

  // What a pull covering every release mounts for the view.
  PulledValues pull() const {
    return backend->animatedPropsRegistry_->pull(
        kSurface, std::numeric_limits<uint64_t>::max(), {kView});
  }
};

TEST_F(AnimationBackendFrontendTest, uncommittedDirectPropsStayOutOfCommits) {
  write(animated, opacity(0.5));
  reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));

  EXPECT_EQ(delegate.synchronousUpdates.at(kView), opacity(0.5));
  EXPECT_FLOAT_EQ(treeOpacity(), 0.8);
}

TEST_F(AnimationBackendFrontendTest, committedDirectPropsAreStamped) {
  write(reanimated, opacity(0.5));
  reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
}

TEST_F(AnimationBackendFrontendTest, unregisteredProducersAreStamped) {
  backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
    return mutations(opacity(0.5));
  });
  reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
}

TEST_F(AnimationBackendFrontendTest, unregisteredProducersRequestEndSyncs) {
  backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
    auto frame = mutations(opacity(0.5));
    frame.asyncFlushSurfaces.insert(kSurface);
    return frame;
  });

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
}

TEST_F(
    AnimationBackendFrontendTest,
    aLayoutCommitLeavesAnotherFrontendsUncommittedValuesOut) {
  startFrontend(false, [&](AnimationTimestamp /*timestamp*/) {
    return mutations(opacity(0.5));
  });
  startFrontend(true, [&](AnimationTimestamp /*timestamp*/) {
    return mutations(folly::dynamic::object("width", 100), true);
  });
  backend->onAnimationFrame(AnimationTimestamp{0});

  auto& props = static_cast<const ViewProps&>(*treeProps());
  EXPECT_EQ(
      props.yogaStyle.dimension(yoga::Dimension::Width),
      yoga::StyleSizeLength::points(100));
  EXPECT_FLOAT_EQ(props.opacity, 0.8);
  EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], 0.5);
}

TEST_F(AnimationBackendFrontendTest, aReleaseGivesTheViewFreshProps) {
  write(animated, opacity(0.5));
  auto props = treeProps();
  release(animated, false);

  EXPECT_NE(treeProps(), props);
  EXPECT_FLOAT_EQ(treeOpacity(), 0.8);
}

TEST_F(AnimationBackendFrontendTest, aReleaseOfAnotherFrontendIsANoOp) {
  write(animated, opacity(0.5));
  auto props = treeProps();
  release(reanimated, false);

  EXPECT_EQ(treeProps(), props);
}

TEST_F(AnimationBackendFrontendTest, anAdoptingReleaseCommitsTheValue) {
  write(animated, opacity(0.5));
  release(animated, true);

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
}

TEST_F(AnimationBackendFrontendTest, aCallbacksReleaseFollowsItsMutations) {
  backend->pushAnimationMutations(
      animated, [&](AnimationTimestamp /*timestamp*/) {
        release(animated, true);
        return mutations(opacity(0.5));
      });

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
}

TEST_F(AnimationBackendFrontendTest, aCallbacksReleaseEndsEarlierValues) {
  write(animated, opacity(0.5));
  backend->pushAnimationMutations(
      animated, [&](AnimationTimestamp /*timestamp*/) {
        release(animated, false);
        return mutations(opacity(0.6));
      });

  auto pulled = pull();
  EXPECT_EQ(pulled.overlaid.at(kView), opacity(0.6));
  EXPECT_TRUE(pulled.restored.empty());
}

TEST_F(AnimationBackendFrontendTest, aCancelledReleaseIsRetriedNextFrame) {
  write(animated, opacity(0.5));
  cancellingCommitHook.cancelsEndSyncs = true;
  release(animated, true);
  EXPECT_FLOAT_EQ(treeOpacity(), 0.8);

  cancellingCommitHook.cancelsEndSyncs = false;
  backend->onAnimationFrame(AnimationTimestamp{0});

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
}

TEST_F(AnimationBackendFrontendTest, cxxAnimatedKeepsItsValuesUncommitted) {
  auto nodesManager = NativeAnimatedNodesManager::create(backend);
  auto family =
      std::const_pointer_cast<ShadowNodeFamily>(view->getFamilyShared());
  nodesManager->scheduleOnUI([nodesManager, family]() {
    nodesManager->createAnimatedNode(
        1, folly::dynamic::object("type", "value")("value", 1)("offset", 0));
    nodesManager->createAnimatedNode(
        2,
        folly::dynamic::object("type", "style")(
            "style", folly::dynamic::object("opacity", 1)));
    nodesManager->createAnimatedNode(
        3,
        folly::dynamic::object("type", "props")(
            "props", folly::dynamic::object("style", 2)));
    nodesManager->connectAnimatedNodes(1, 2);
    nodesManager->connectAnimatedNodes(2, 3);
    nodesManager->connectAnimatedNodeToView(3, kView);
    nodesManager->connectAnimatedNodeToShadowNodeFamily(3, family);
    nodesManager->startAnimatingNode(
        1,
        1,
        folly::dynamic::object("type", "frames")(
            "frames", folly::dynamic::array(0.0, 0.5, 1.0))("toValue", 0.2),
        std::nullopt);
  });

  backend->onAnimationFrame(AnimationTimestamp{1000});
  reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));
  EXPECT_EQ(delegate.synchronousUpdates.at(kView), opacity(1));
  EXPECT_FLOAT_EQ(treeOpacity(), 0.8);

  for (int frame = 1; frame < 5; frame++) {
    backend->onAnimationFrame(AnimationTimestamp{1000 + frame * 17.0});
  }
  EXPECT_FLOAT_EQ(treeOpacity(), 0.2);
}

TEST_F(AnimationBackendFrontendTest, aDisconnectRestoresReactsValue) {
  connectNode(1, "opacity", 0.5);
  frame();
  disconnectNode(1);
  frame();

  auto pulled = pull();
  EXPECT_EQ(pulled.restored.at(kView), opacity(0.5));
  EXPECT_TRUE(pulled.overlaid.empty());
}

TEST_F(
    AnimationBackendFrontendTest,
    restoringDefaultValuesRestoresReactsValue) {
  connectNode(1, "opacity", 0.5);
  frame();
  nodesManager->scheduleOnUI(
      [manager = nodesManager]() { manager->restoreDefaultValues(3); });
  frame();

  EXPECT_EQ(pull().restored.at(kView), opacity(0.5));
}

TEST_F(AnimationBackendFrontendTest, aStaleDisconnectKeepsANewerNodesValue) {
  connectNode(1, "opacity", 0.5);
  frame();
  connectNode(4, "opacity", 0.7);
  frame();
  disconnectNode(1);
  frame();

  auto pulled = pull();
  EXPECT_EQ(pulled.overlaid.at(kView), opacity(0.7));
  EXPECT_TRUE(pulled.restored.empty());
}

TEST_F(AnimationBackendFrontendTest, aReconnectionInOneFrameKeepsTheNewValue) {
  connectNode(1, "opacity", 0.5);
  frame();
  disconnectNode(1);
  connectNode(4, "opacity", 0.7);
  frame();

  auto pulled = pull();
  EXPECT_EQ(pulled.overlaid.at(kView), opacity(0.7));
  EXPECT_TRUE(pulled.restored.empty());
}

TEST_F(
    AnimationBackendFrontendTest,
    aFinishedNodeAdoptsOnlyItsValuesOfTheView) {
  connectNode(1, "opacity", 1);
  connectNode(4, "borderRadius", 0);
  nodesManager->scheduleOnUI([manager = nodesManager]() {
    manager->startAnimatingNode(
        1,
        1,
        folly::dynamic::object("type", "frames")(
            "frames", folly::dynamic::array(0.0, 0.5, 1.0))("toValue", 0.2),
        std::nullopt);
    auto frames = folly::dynamic::array();
    for (int index = 0; index <= 100; index++) {
      frames.push_back(index / 100.0);
    }
    manager->startAnimatingNode(
        2,
        4,
        folly::dynamic::object("type", "frames")("frames", frames)(
            "toValue", 10),
        std::nullopt);
  });
  for (int index = 0; index < 10 && treeOpacity() != 0.2f; index++) {
    frame();
  }

  EXPECT_FLOAT_EQ(treeOpacity(), 0.2);
  auto overlaid = pull().overlaid.at(kView);
  EXPECT_TRUE(overlaid.count("borderRadius"));
  EXPECT_FALSE(overlaid.count("opacity"));
}

TEST_F(AnimationBackendFrontendTest, anAdoptedValueIsNotCommittedAgain) {
  jsInvoker->defers = true;
  write(animated, opacity(0.5));
  release(animated, true);
  jsInvoker->runTasks();
  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);

  reactCommit(opacity(0.2));
  backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
    AnimationMutations frame;
    frame.asyncFlushSurfaces.insert(kSurface);
    return frame;
  });
  jsInvoker->runTasks();

  EXPECT_FLOAT_EQ(treeOpacity(), 0.2);
}

TEST_F(AnimationBackendFrontendTest, anotherFrontendsWriteEndsAnAdoption) {
  AnimationFrontendId first{};
  first = startFrontend(false, [&](AnimationTimestamp /*timestamp*/) {
    release(first, true);
    return mutations(opacity(0.5));
  });
  startFrontend(false, [&](AnimationTimestamp /*timestamp*/) {
    return mutations(opacity(0.7));
  });
  frame();

  EXPECT_FLOAT_EQ(treeOpacity(), 0.8);
  EXPECT_EQ(pull().overlaid.at(kView), opacity(0.7));
}

TEST_F(AnimationBackendFrontendTest, aReentrantWriteEndsAnAdoption) {
  delegate.onSynchronousUpdate = [&]() { write(animated, opacity(0.7)); };
  backend->pushAnimationMutations(
      animated, [&](AnimationTimestamp /*timestamp*/) {
        release(animated, true);
        return mutations(opacity(0.5));
      });

  EXPECT_FLOAT_EQ(treeOpacity(), 0.8);
  EXPECT_EQ(pull().overlaid.at(kView), opacity(0.7));
}

TEST_F(AnimationBackendFrontendTest, aLaterRawValueWinsOverATypedOne) {
  overrideFlags(false, false);
  expectLaterRawValueWins();
}

TEST_F(AnimationBackendFrontendTest, aLaterRawValueWinsInABatchedWrite) {
  overrideFlags(true, false);
  expectLaterRawValueWins();
}

TEST_F(AnimationBackendFrontendTest, aLaterTypedValueWinsOverARawOne) {
  overrideFlags(false, false);
  expectLaterTypedValueWins();
}

TEST_F(AnimationBackendFrontendTest, aLaterTypedValueWinsInABatchedWrite) {
  overrideFlags(true, false);
  expectLaterTypedValueWins();
}

TEST_F(AnimationBackendFrontendTest, releasesOfASurfaceShareOneCommit) {
  jsInvoker->defers = true;
  write(animated, opacity(0.5));
  backend->pushAnimationMutations(
      animated, [&](AnimationTimestamp /*timestamp*/) {
        release(animated, true);
        release(animated, true, "backgroundColor");
        release(animated, true, "transform");
        return AnimationMutations{};
      });
  release(animated, false);

  EXPECT_EQ(jsInvoker->tasks.size(), 1u);
}

TEST_F(AnimationBackendFrontendTest, aCancelledReleaseKeepsFramesComing) {
  write(animated, opacity(0.5));
  cancellingCommitHook.cancelsEndSyncs = true;
  release(animated, true);
  EXPECT_TRUE(choreographer->isResumed);

  cancellingCommitHook.cancelsEndSyncs = false;
  frame();

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
  EXPECT_FALSE(choreographer->isResumed);
}

TEST_F(AnimationBackendFrontendTest, aStartedFrontendRunsOncePerFrame) {
  int frames = 0;
  auto frontendId = startFrontend(true, [&](AnimationTimestamp /*timestamp*/) {
    frames++;
    return AnimationMutations{};
  });
  backend->start(frontendId);
  frame();
  EXPECT_EQ(frames, 1);

  backend->stop(frontendId);
  backend->stop(frontendId);
  frame();
  EXPECT_EQ(frames, 1);

  backend->start(frontendId);
  frame();
  EXPECT_EQ(frames, 2);
}

TEST_F(AnimationBackendFrontendTest, framesComeWhileAFrontendIsStarted) {
  auto idle = [](AnimationTimestamp /*timestamp*/) {
    return AnimationMutations{};
  };
  auto first = startFrontend(true, idle);
  auto second = startFrontend(false, idle);
  EXPECT_TRUE(choreographer->isResumed);
  backend->stop(first);
  EXPECT_TRUE(choreographer->isResumed);
  backend->stop(second);
  EXPECT_FALSE(choreographer->isResumed);

  auto callbackId = backend->start(idle);
  backend->start(first);
  backend->stop(first);
  EXPECT_TRUE(choreographer->isResumed);
  backend->stop(callbackId);
  EXPECT_FALSE(choreographer->isResumed);

  write(animated, opacity(0.5));
  cancellingCommitHook.cancelsEndSyncs = true;
  release(animated, true);
  backend->start(first);
  backend->stop(first);
  EXPECT_TRUE(choreographer->isResumed);
  cancellingCommitHook.cancelsEndSyncs = false;
  frame();
  EXPECT_FALSE(choreographer->isResumed);
}

TEST_F(AnimationBackendFrontendTest, framesRunInRegistrationOrder) {
  auto first = backend->registerFrontend(
      {.onAnimationFrame = [&](AnimationTimestamp /*timestamp*/) {
        return mutations(opacity(0.3));
      }});
  auto second = backend->registerFrontend(
      {.onAnimationFrame = [&](AnimationTimestamp /*timestamp*/) {
        return mutations(opacity(0.5));
      }});
  backend->start(second);
  backend->start(first);
  auto legacy = backend->start([&](AnimationTimestamp /*timestamp*/) {
    return mutations(opacity(0.7));
  });
  frame();
  backend->stop(legacy);

  EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], 0.5);
}

TEST_F(
    AnimationBackendFrontendTest,
    aFrontendWithoutAFrameCallbackIsNotStarted) {
  auto frontendId = backend->registerFrontend({.commitDirectProps = true});
  backend->start(frontendId);
  EXPECT_FALSE(choreographer->isResumed);
  frame();
}

TEST_F(
    AnimationBackendFrontendTest,
    aManagerDestroyedAfterItsCallbackWasCopiedWritesNothing) {
  connectNode(1, "opacity", 0.5);
  frame();
  std::weak_ptr<NativeAnimatedNodesManager> manager = nodesManager;
  auto destroyManager = backend->start([&](AnimationTimestamp /*timestamp*/) {
    nodesManager.reset();
    return AnimationMutations{};
  });
  delegate.synchronousUpdates.clear();
  frame();
  backend->stop(destroyManager);

  EXPECT_TRUE(manager.expired());
  EXPECT_TRUE(delegate.synchronousUpdates.empty());
}

TEST_F(AnimationBackendFrontendTest, cxxAnimatedStopsWhenIdleAndRestarts) {
  connectNode(1, "opacity", 0.5);
  EXPECT_TRUE(choreographer->isResumed);
  frame();
  frame();
  EXPECT_FALSE(choreographer->isResumed);

  nodesManager->scheduleOnUI(
      [manager = nodesManager]() { manager->setAnimatedNodeValue(1, 0.3); });
  EXPECT_TRUE(choreographer->isResumed);
  frame();
  EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], 0.3);
}

TEST_F(AnimationBackendFrontendTest, aCancelledEndSyncIsNotRetried) {
  cancellingCommitHook.cancelsEndSyncs = true;
  backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
    auto frame = mutations(opacity(0.5));
    frame.asyncFlushSurfaces.insert(kSurface);
    return frame;
  });

  EXPECT_FALSE(choreographer->isResumed);
}

TEST_F(AnimationBackendFrontendTest, releasesKeepUnregisteredStampedValues) {
  overrideFlags(false, true);
  backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
    return mutations(opacity(0.5));
  });
  write(animated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  release(animated, false, "backgroundColor");

  ASSERT_NE(stampedSnapshot(), nullptr);
  EXPECT_EQ(stampedSnapshot()->rawProps->at("opacity"), 0.5);

  release(animated, true);
  EXPECT_EQ(stampedSnapshot()->rawProps->at("opacity"), 0.5);
}

TEST_F(AnimationBackendFrontendTest, aLaterCommittedValueIsStamped) {
  for (bool rawFirst : {false, true}) {
    for (bool isCommittedBetween : {false, true}) {
      writeCommittedOpacities(rawFirst, isCommittedBetween);

      EXPECT_FLOAT_EQ(treeOpacity(), rawFirst ? 0.3 : 0.5)
          << "rawFirst " << rawFirst << ", committed between "
          << isCommittedBetween;
    }
  }
}

TEST_F(AnimationBackendFrontendTest, anAdoptedBorderKeyKeepsItsGroupsValues) {
  for (bool isCommittedBetween : {false, true}) {
    backend->pushAnimationMutations(
        reanimated, [&](AnimationTimestamp /*timestamp*/) {
          return typedMutations(typedBorderRadii());
        });
    if (isCommittedBetween) {
      reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));
    }
    write(animated, folly::dynamic::object("borderTopLeftRadius", 5));
    release(animated, true, "borderTopLeftRadius");

    EXPECT_EQ(treeBorderRadii().topLeft, ValueUnit(5, UnitType::Point));
    EXPECT_EQ(treeBorderRadii().topRight, ValueUnit(50, UnitType::Percent));
  }
}

TEST_F(AnimationBackendFrontendTest, aRestoredBorderKeyKeepsItsGroupsValues) {
  backend->pushAnimationMutations(
      reanimated, [&](AnimationTimestamp /*timestamp*/) {
        return typedMutations(typedBorderRadii());
      });
  write(animated, folly::dynamic::object("borderTopLeftRadius", 5));
  release(animated, false, "borderTopLeftRadius");

  EXPECT_EQ(treeBorderRadii().topLeft, std::nullopt);
  EXPECT_EQ(treeBorderRadii().topRight, ValueUnit(50, UnitType::Percent));
}

TEST_F(
    AnimationBackendFrontendTest,
    aLaterCommittedValueStaysInTheLayoutCommit) {
  startFrontend(false, [&](AnimationTimestamp /*timestamp*/) {
    return mutations(opacity(0.5));
  });
  startFrontend(true, [&](AnimationTimestamp /*timestamp*/) {
    return mutations(
        folly::dynamic::object("opacity", 0.3)("width", 100), true);
  });
  frame();

  EXPECT_FLOAT_EQ(treeOpacity(), 0.3);
}

TEST_F(AnimationBackendFrontendTest, aLayoutCommitKeepsPercentRadii) {
  startFrontend(true, [&](AnimationTimestamp /*timestamp*/) {
    auto props = typedBorderRadii();
    props.rawProps =
        std::make_unique<RawProps>(folly::dynamic::object("width", 100));
    auto frame = typedMutations(std::move(props));
    frame.batch.back().hasLayoutUpdates = true;
    return frame;
  });
  startFrontend(false, [&](AnimationTimestamp /*timestamp*/) {
    return mutations(folly::dynamic::object("borderTopLeftRadius", 5));
  });
  frame();

  EXPECT_EQ(treeBorderRadii().topLeft, std::nullopt);
  EXPECT_EQ(treeBorderRadii().topRight, ValueUnit(50, UnitType::Percent));
}

TEST_F(AnimationBackendFrontendTest, aSatisfiedCancelledReleaseIsNotRetried) {
  overrideFlags(false, true);
  jsInvoker->defers = true;
  write(animated, opacity(0.5));
  cancellingCommitHook.cancelsEndSyncs = true;
  release(animated, true);
  jsInvoker->runTasks();
  cancellingCommitHook.cancelsEndSyncs = false;
  write(animated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  release(animated, true, "backgroundColor");
  jsInvoker->runTasks();
  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);

  backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
    return mutations(folly::dynamic::object("shadowOpacity", 0.4));
  });
  frame();

  EXPECT_TRUE(jsInvoker->tasks.empty());
  ASSERT_NE(stampedSnapshot(), nullptr);
  EXPECT_EQ(stampedSnapshot()->rawProps->at("shadowOpacity"), 0.4);
}

TEST_F(AnimationBackendFrontendTest, aRetryJoinsAQueuedReleaseCommit) {
  overrideFlags(false, true);
  jsInvoker->defers = true;
  write(animated, opacity(0.5));
  cancellingCommitHook.cancelsEndSyncs = true;
  release(animated, true);
  jsInvoker->runTasks();
  cancellingCommitHook.cancelsEndSyncs = false;
  write(animated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  release(animated, true, "backgroundColor");
  backend->start([&](AnimationTimestamp /*timestamp*/) {
    jsInvoker->runTasks();
    return mutations(folly::dynamic::object("shadowOpacity", 0.4));
  });
  frame();

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
  EXPECT_TRUE(jsInvoker->tasks.empty());
  ASSERT_NE(stampedSnapshot(), nullptr);
  EXPECT_EQ(stampedSnapshot()->rawProps->at("shadowOpacity"), 0.4);
}

TEST_F(AnimationBackendFrontendTest, anAdoptedBorderEdgeKeepsItsGroupsEdges) {
  for (bool isCommittedBetween : {false, true}) {
    CascadedBorderStyles borderStyles;
    borderStyles.horizontal = BorderStyle::Dashed;
    borderStyles.right = BorderStyle::Solid;
    writeTyped(BORDER_STYLES, borderStyles);
    if (isCommittedBetween) {
      reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));
    }
    write(animated, folly::dynamic::object("borderRightStyle", "dotted"));
    release(animated, true, "borderRightStyle");

    EXPECT_EQ(viewProps().borderStyles.horizontal, BorderStyle::Dashed);
    EXPECT_EQ(viewProps().borderStyles.right, BorderStyle::Dotted);
  }
}

TEST_F(
    AnimationBackendFrontendTest,
    anAdoptedBorderCurveKeepsItsLogicalCurves) {
  for (bool isCommittedBetween : {false, true}) {
    CascadedBorderCurves borderCurves;
    borderCurves.topStart = BorderCurve::Continuous;
    borderCurves.topLeft = BorderCurve::Circular;
    writeTyped(BORDER_CURVES, borderCurves);
    if (isCommittedBetween) {
      reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));
    }
    write(animated, folly::dynamic::object("borderTopLeftCurve", "continuous"));
    release(animated, true, "borderTopLeftCurve");

    EXPECT_EQ(viewProps().borderCurves.topStart, BorderCurve::Continuous);
    EXPECT_EQ(viewProps().borderCurves.topLeft, BorderCurve::Continuous);
  }
}

TEST_F(
    AnimationBackendFrontendTest,
    aTypedBorderGroupSupersedesAnEarlierRawValueOfItsGroup) {
  backend->start([&](AnimationTimestamp /*timestamp*/) {
    return mutations(folly::dynamic::object("borderTopLeftRadius", 5));
  });
  backend->start([&](AnimationTimestamp /*timestamp*/) {
    CascadedBorderRadii borderRadii;
    borderRadii.topRight = ValueUnit(9, UnitType::Point);
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setBorderRadii(borderRadii);
    return typedMutations(propsBuilder.get());
  });
  backend->start([&](AnimationTimestamp /*timestamp*/) {
    return mutations(folly::dynamic::object("borderTopRightRadius", 11));
  });
  frame();
  reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));

  EXPECT_EQ(
      delegate.synchronousUpdates.at(kView),
      folly::dynamic(folly::dynamic::object("borderTopRightRadius", 11)));
  EXPECT_EQ(treeBorderRadii().topLeft, std::nullopt);
  EXPECT_EQ(treeBorderRadii().topRight, ValueUnit(11, UnitType::Point));
}

TEST_F(
    AnimationBackendFrontendTest,
    aConvertedBorderGroupNoLongerResetsUnsetSiblings) {
  reactCommit(folly::dynamic::object("borderBottomLeftRadius", 12));
  backend->pushAnimationMutations(
      reanimated, [&](AnimationTimestamp /*timestamp*/) {
        return typedMutations(typedBorderRadii());
      });
  reactCommit(folly::dynamic::object("borderBottomLeftRadius", 12));
  EXPECT_EQ(treeBorderRadii().bottomLeft, std::nullopt);

  write(animated, folly::dynamic::object("borderTopLeftRadius", 5));
  reactCommit(folly::dynamic::object("borderBottomLeftRadius", 12));

  EXPECT_EQ(treeBorderRadii().bottomLeft, ValueUnit(12, UnitType::Point));
  EXPECT_EQ(treeBorderRadii().topRight, ValueUnit(50, UnitType::Percent));
}

TEST_F(
    AnimationBackendFrontendTest,
    aReplacedTypedGroupDoesNotStampItsValuesLater) {
  auto writeRadius = [&](std::optional<ValueUnit> CascadedBorderRadii::* corner,
                         Float value) {
    CascadedBorderRadii borderRadii;
    borderRadii.*corner = ValueUnit(value, UnitType::Point);
    writeTyped(BORDER_RADII, borderRadii);
  };
  for (bool isCommittedBetween : {false, true}) {
    for (bool isTakenOver : {false, true}) {
      writeRadius(&CascadedBorderRadii::topLeft, 5);
      reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));
      writeRadius(&CascadedBorderRadii::topRight, 9);
      if (isCommittedBetween) {
        reactCommit(folly::dynamic::object("backgroundColor", 0xff00ff00));
      }
      folly::dynamic rawRadius =
          folly::dynamic::object("borderTopRightRadius", 11);
      if (isTakenOver) {
        write(animated, rawRadius);
      } else {
        backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
          return mutations(rawRadius);
        });
      }
      reactCommit(folly::dynamic::object("borderTopLeftRadius", 20));

      EXPECT_EQ(treeBorderRadii().topLeft, ValueUnit(20, UnitType::Point))
          << "committed between " << isCommittedBetween << ", taken over "
          << isTakenOver;
    }
  }
}

TEST_F(
    AnimationBackendFrontendTest,
    aLaterTypedGroupReplacesAnEarlierOneInAFrame) {
  for (auto [corner, value] : std::vector<
           std::pair<std::optional<ValueUnit> CascadedBorderRadii::*, Float>>{
           {&CascadedBorderRadii::topLeft, 5},
           {&CascadedBorderRadii::topRight, 9}}) {
    backend->start([&, corner, value](AnimationTimestamp /*timestamp*/) {
      CascadedBorderRadii borderRadii;
      borderRadii.*corner = ValueUnit(value, UnitType::Point);
      AnimatedPropsBuilder propsBuilder;
      propsBuilder.setBorderRadii(borderRadii);
      return typedMutations(propsBuilder.get());
    });
  }
  backend->start([&](AnimationTimestamp /*timestamp*/) {
    return mutations(folly::dynamic::object("borderTopRightRadius", 11));
  });
  frame();

  folly::dynamic expected = folly::dynamic::object("borderTopRightRadius", 11);
  EXPECT_EQ(delegate.synchronousUpdates.at(kView), expected);
  EXPECT_EQ(*stampedSnapshot()->rawProps, expected);
  EXPECT_FALSE(stampedSnapshot()->propNames.contains(BORDER_RADII));
  reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));
  EXPECT_EQ(treeBorderRadii().topLeft, std::nullopt);
  EXPECT_EQ(treeBorderRadii().topRight, ValueUnit(11, UnitType::Point));
}

TEST_F(
    AnimationBackendFrontendTest,
    aRegisteredStampedValueWinsUntilItIsAdopted) {
  overrideFlags(false, true);
  write(reanimated, opacity(0.5));
  reactCommit(opacity(0.2));
  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);

  release(reanimated, true);
  reactCommit(opacity(0.2));

  EXPECT_FLOAT_EQ(treeOpacity(), 0.2);
}

TEST_F(AnimationBackendFrontendTest, anAdoptionStopsStampingOnlyItsKeys) {
  overrideFlags(false, true);
  write(
      reanimated,
      folly::dynamic::object("opacity", 0.5)("backgroundColor", 0xff0000ff));
  release(reanimated, true);

  EXPECT_FLOAT_EQ(treeOpacity(), 0.5);
  EXPECT_EQ(
      *stampedSnapshot()->rawProps,
      folly::dynamic(folly::dynamic::object("backgroundColor", 0xff0000ff)));
}

TEST_F(AnimationBackendFrontendTest, anAdoptedTypedValueStopsStamping) {
  overrideFlags(false, true);
  writeTyped(OPACITY, Float{0.5});
  release(reanimated, true);
  reactCommit(opacity(0.2));

  EXPECT_EQ(stampedSnapshot(), nullptr);
  EXPECT_FLOAT_EQ(treeOpacity(), 0.2);
}

TEST_F(AnimationBackendFrontendTest, aStampedRestoreShowsTheRestoredValue) {
  overrideFlags(false, true);
  for (auto [restored, shown] : std::vector<std::pair<folly::dynamic, Float>>{
           {0.8, 0.8}, {nullptr, 1}}) {
    write(reanimated, opacity(0.5));
    release(
        reanimated,
        false,
        "opacity",
        folly::dynamic::object("opacity", restored));

    EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], restored);
    EXPECT_FLOAT_EQ(treeOpacity(), shown);
    EXPECT_EQ(stampedSnapshot(), nullptr);
  }
}

TEST_F(AnimationBackendFrontendTest, aCallbacksRestoreFollowsItsOrder) {
  overrideFlags(false, true);
  write(reanimated, opacity(0.5));
  backend->pushAnimationMutations(
      reanimated, [&](AnimationTimestamp /*timestamp*/) {
        release(
            reanimated,
            true,
            "opacity",
            folly::dynamic::object("opacity", 0.2));
        return mutations(folly::dynamic::object("backgroundColor", 0xff0000ff));
      });

  EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], 0.2);
  EXPECT_FLOAT_EQ(treeOpacity(), 0.2);
}

TEST_F(AnimationBackendFrontendTest, aRestoreKeepsANewerFrontendsValue) {
  overrideFlags(false, true);
  auto otherReanimated = backend->registerFrontend({.commitDirectProps = true});
  for (auto newerWriter : {otherReanimated, animated}) {
    write(reanimated, opacity(0.5));
    write(newerWriter, opacity(0.8));
    release(
        reanimated, false, "opacity", folly::dynamic::object("opacity", 0.2));
    reactCommit(folly::dynamic::object("backgroundColor", 0xff0000ff));

    EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], 0.8);
    if (newerWriter == animated) {
      EXPECT_EQ(pull().overlaid.at(kView), opacity(0.8));
    } else {
      EXPECT_FLOAT_EQ(treeOpacity(), 0.8);
    }
  }
}

TEST_F(AnimationBackendFrontendTest, aRestoreKeepsAKeyTakenOverFromItsGroup) {
  overrideFlags(false, true);
  for (bool isTyped : {false, true}) {
    for (bool isConsumedBetween : {false, true}) {
      if (isTyped) {
        backend->pushAnimationMutations(
            reanimated, [&](AnimationTimestamp /*timestamp*/) {
              return typedMutations(typedBorderRadii());
            });
      } else {
        write(
            reanimated,
            folly::dynamic::object("borderTopLeftRadius", 3)(
                "borderTopRightRadius", 9));
      }
      if (isConsumedBetween) {
        stampedSnapshot();
      }
      write(animated, folly::dynamic::object("borderTopLeftRadius", 7));
      delegate.synchronousUpdates.clear();
      backend->release(
          reanimated,
          {{.family = view->getFamilyShared(),
            .keys = {"borderTopLeftRadius", "borderTopRightRadius"},
            .commitValues = true,
            .restoredValues = folly::dynamic::object("borderTopLeftRadius", 0)(
                "borderTopRightRadius", 0)}});

      folly::dynamic restored =
          folly::dynamic::object("borderTopRightRadius", 0);
      folly::dynamic takenOver =
          folly::dynamic::object("borderTopLeftRadius", 7);
      EXPECT_EQ(delegate.synchronousUpdates.at(kView), restored);
      EXPECT_EQ(pull().overlaid.at(kView), takenOver);
      EXPECT_EQ(treeBorderRadii().topRight, ValueUnit(0, UnitType::Point));
    }
  }
}

TEST_F(AnimationBackendFrontendTest, aRestoreBesideAnEmptyUncommittedWrite) {
  overrideFlags(false, true);
  for (bool hasRawProps : {true, false}) {
    write(reanimated, opacity(0.5));
    if (hasRawProps) {
      write(animated, folly::dynamic::object());
    } else {
      backend->pushAnimationMutations(
          animated, [&](AnimationTimestamp /*timestamp*/) {
            return typedMutations(AnimatedProps{});
          });
    }
    release(reanimated, true, "opacity", opacity(0.8));

    EXPECT_EQ(delegate.synchronousUpdates.at(kView)["opacity"], 0.8);
    EXPECT_FLOAT_EQ(treeOpacity(), 0.8);
    EXPECT_TRUE(pull().overlaid.empty());
  }
}

TEST_F(AnimationBackendFrontendTest, aWriteAfterAnAdoptionKeepsStamping) {
  overrideFlags(false, true);
  jsInvoker->defers = true;
  write(reanimated, opacity(0.5));
  release(reanimated, true);
  write(reanimated, opacity(0.6));
  jsInvoker->runTasks();

  EXPECT_EQ(stampedSnapshot()->rawProps->at("opacity"), 0.6);
}

TEST_F(AnimationBackendFrontendTest, anotherFrontendsStampedValueIsKept) {
  overrideFlags(false, true);
  jsInvoker->defers = true;
  write(reanimated, opacity(0.5));
  release(animated, true);

  EXPECT_TRUE(jsInvoker->tasks.empty());
  EXPECT_EQ(stampedSnapshot()->rawProps->at("opacity"), 0.5);
}

TEST_F(AnimationBackendFrontendTest, registeredFrontendsRequestNoEndSyncs) {
  jsInvoker->defers = true;
  backend->pushAnimationMutations(
      reanimated, [&](AnimationTimestamp /*timestamp*/) {
        auto frame = mutations(opacity(0.5));
        frame.asyncFlushSurfaces.insert(kSurface);
        return frame;
      });

  EXPECT_TRUE(jsInvoker->tasks.empty());
}

TEST_F(
    AnimationBackendFrontendTest,
    aReactCommitClearsOnlyUnregisteredStampedValues) {
  backend->pushAnimationMutations([&](AnimationTimestamp /*timestamp*/) {
    return mutations(opacity(0.5));
  });
  write(reanimated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  stampedSnapshot();
  backend->clearRegistry(kSurface);

  EXPECT_EQ(
      *stampedSnapshot()->rawProps,
      folly::dynamic(folly::dynamic::object("backgroundColor", 0xff0000ff)));
}

TEST_F(
    AnimationBackendFrontendTest,
    aStampedAdoptionCommitsBesideUncommittedValues) {
  overrideFlags(false, true);
  write(reanimated, opacity(0.5));
  write(animated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  release(reanimated, true);

  EXPECT_EQ(stampedSnapshot(), nullptr);
  EXPECT_EQ(
      pull().overlaid.at(kView),
      folly::dynamic(folly::dynamic::object("backgroundColor", 0xff0000ff)));
}

TEST_F(
    AnimationBackendFrontendTest,
    aStampedAdoptionCommitsBesideACommittedRelease) {
  overrideFlags(false, true);
  write(animated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  release(animated, true, "backgroundColor");
  write(reanimated, opacity(0.5));
  release(reanimated, true);

  EXPECT_EQ(stampedSnapshot(), nullptr);
}

TEST_F(
    AnimationBackendFrontendTest,
    aReleasedStampedValueEndsWithAReactCommit) {
  uiManager->unstable_setAnimationBackend(backend);
  for (bool updatesRuntimeReferences : {true, false}) {
    overrideFlags(false, updatesRuntimeReferences, true);
    write(reanimated, opacity(0.5));
    release(reanimated, true);

    completeSurface(opacity(0.2));
    EXPECT_FLOAT_EQ(treeOpacity(), updatesRuntimeReferences ? 0.2 : 0.5);
    EXPECT_EQ(stampedSnapshot(), nullptr);

    completeSurface(opacity(0.2));
    EXPECT_FLOAT_EQ(treeOpacity(), 0.2);
  }
}

TEST_F(AnimationBackendFrontendTest, aGroupIsOwnedByItsLastWriter) {
  overrideFlags(false, true);
  auto otherReanimated = backend->registerFrontend({.commitDirectProps = true});
  write(reanimated, folly::dynamic::object("borderTopLeftRadius", 3));
  write(otherReanimated, folly::dynamic::object("borderTopRightRadius", 9));
  release(reanimated, true, "borderTopLeftRadius");
  EXPECT_EQ(stampedSnapshot()->rawProps->size(), 2u);

  release(otherReanimated, true, "borderTopRightRadius");
  EXPECT_EQ(stampedSnapshot(), nullptr);
}

TEST_F(AnimationBackendFrontendTest, aSettledGroupKeyReleasesTheGroup) {
  overrideFlags(false, true);
  write(
      reanimated,
      folly::dynamic::object("borderTopLeftRadius", 3)(
          "borderTopRightRadius", 9));
  release(reanimated, true, "borderTopLeftRadius");

  EXPECT_EQ(stampedSnapshot(), nullptr);
}

TEST_F(AnimationBackendFrontendTest, aWriteDuringTheAdoptionKeepsStamping) {
  overrideFlags(false, true);
  write(reanimated, opacity(0.5));
  cancellingCommitHook.duringEndSync = [&]() {
    write(reanimated, opacity(0.6));
  };
  release(reanimated, true);

  EXPECT_EQ(stampedSnapshot()->rawProps->at("opacity"), 0.6);
}

TEST_F(AnimationBackendFrontendTest, aReleaseDuringTheAdoptionWaitsForItsOwn) {
  overrideFlags(false, true);
  jsInvoker->defers = true;
  write(reanimated, opacity(0.5));
  release(reanimated, true);
  cancellingCommitHook.duringEndSync = [&]() { release(reanimated, true); };
  jsInvoker->runTasks();
  EXPECT_EQ(stampedSnapshot()->rawProps->at("opacity"), 0.5);

  jsInvoker->runTasks();
  EXPECT_EQ(stampedSnapshot(), nullptr);
}

} // namespace facebook::react
