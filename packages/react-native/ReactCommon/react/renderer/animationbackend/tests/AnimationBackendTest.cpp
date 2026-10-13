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
#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/animationbackend/AnimationBackend.h>
#include <react/renderer/components/root/RootShadowNode.h>
#include <react/renderer/components/scrollview/ScrollViewShadowNode.h>
#include <react/renderer/components/text/ParagraphShadowNode.h>
#include <react/renderer/components/view/ViewShadowNode.h>
#include <react/renderer/element/ComponentBuilder.h>
#include <react/renderer/element/Element.h>
#include <react/renderer/element/testUtils.h>
#include <react/renderer/graphics/Color.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/uimanager/UIManagerCommitHook.h>
#include <react/renderer/uimanager/UIManagerDelegate.h>
#include <react/utils/ContextContainer.h>
#include <array>

namespace facebook::react {

namespace {

class TestAnimationChoreographer : public AnimationChoreographer {
 public:
  void resume() override {}
  void pause() override {}
};

class RecordingUIManagerDelegate : public UIManagerDelegate {
 public:
  std::unordered_map<Tag, folly::dynamic> synchronousUpdates;
  std::vector<std::unordered_map<Tag, folly::dynamic>> batchedUpdates;

  void uiManagerShouldSynchronouslyUpdateViewOnUIThread(
      Tag tag,
      const folly::dynamic& props) override {
    synchronousUpdates.emplace(tag, props);
  }

  void uiManagerShouldSynchronouslyUpdateAnimatedProps(
      const std::unordered_map<Tag, AnimatedProps>& updates) override {
    auto& batch = batchedUpdates.emplace_back();
    for (const auto& [tag, props] : updates) {
      batch.emplace(tag, animationbackend::packAnimatedProps(props));
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

  void invokeAsync(CallFunc&& /*func*/) noexcept override {}
  void invokeAsync(std::function<void()>&& func) noexcept override {
    if (defers) {
      tasks_.push_back(std::move(func));
    } else {
      func();
    }
  }
  void invokeSync(CallFunc&& /*func*/) override {}

  void runDeferredTasks() {
    defers = false;
    runQueuedTasks();
  }

  size_t queuedTaskCount() const {
    return tasks_.size();
  }

  // Runs the queued tasks; the tasks they queue stay queued while deferring.
  void runQueuedTasks() {
    for (auto& task : std::exchange(tasks_, {})) {
      task();
    }
  }

 private:
  std::vector<std::function<void()>> tasks_;
};

// Pulls cover every revision committed since the last one on Android too.
class AccumulatedPropsFlags : public ReactNativeFeatureFlagsDefaults {
 public:
  bool enableAccumulatedUpdatesInRawPropsAndroid() override {
    return true;
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
  std::shared_ptr<ViewShadowNode> otherNode =
      builder.build(Element<ViewShadowNode>().surfaceId(1).tag(11));
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

TEST_F(AnimationBackendTest, committedViewIsWrittenDirectlyToo) {
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

  EXPECT_TRUE(delegate.synchronousUpdates.empty());
  ASSERT_EQ(delegate.batchedUpdates.size(), 1u);
  EXPECT_EQ(
      delegate.batchedUpdates[0].at(10),
      folly::dynamic(folly::dynamic::object("opacity", 0.5)));
}

TEST_F(AnimationBackendTest, aFrameWritesAllViewsInOneBatch) {
  backend->pushAnimationMutations([this](AnimationTimestamp /*timestamp*/) {
    AnimationMutations mutations;
    for (const auto& view : {node, otherNode}) {
      AnimatedPropsBuilder propsBuilder;
      propsBuilder.setOpacity(0.5);
      mutations.batch.push_back(
          AnimationMutation{
              .tag = view->getTag(),
              .family = view->getFamilyShared(),
              .props = propsBuilder.get()});
    }
    return mutations;
  });

  EXPECT_TRUE(delegate.synchronousUpdates.empty());
  ASSERT_EQ(delegate.batchedUpdates.size(), 1u);
  EXPECT_EQ(delegate.batchedUpdates[0].size(), 2u);
}

// A view on a running surface whose React opacity is 0.8.
class AnimationBackendSurfaceTest : public ::testing::Test {
 protected:
  std::shared_ptr<ContextContainer> contextContainer =
      std::make_shared<ContextContainer>();
  ComponentBuilder builder = simpleComponentBuilder(contextContainer);
  RecordingUIManagerDelegate delegate;
  std::shared_ptr<UIManager> uiManager = std::make_shared<UIManager>(
      [](std::function<void(jsi::Runtime&)>&& /*callback*/) {},
      contextContainer);
  std::shared_ptr<ViewShadowNode> view;
  ShadowTree* shadowTree{nullptr};
  std::shared_ptr<AnimationBackend> backend;
  std::shared_ptr<TestCallInvoker> jsInvoker =
      std::make_shared<TestCallInvoker>();

  // The root's child, which shows `view`.
  virtual std::shared_ptr<const ShadowNode> buildRootChild() {
    view = builder.build(
        Element<ViewShadowNode>().tag(kView).surfaceId(kSurface).props([] {
          auto props = std::make_shared<ViewProps>();
          props->opacity = 0.8;
          return props;
        }));
    return view;
  }

  void SetUp() override {
    uiManager->setDelegate(&delegate);
    auto rootChild = buildRootChild();
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
                       std::vector<std::shared_ptr<const ShadowNode>>{
                           rootChild})}));
        },
        {});
    uiManager->startSurface(
        std::move(tree), "test", folly::dynamic::object, DisplayMode::Visible);
    pull();
    backend = std::make_shared<AnimationBackend>(
        std::make_shared<TestAnimationChoreographer>(), uiManager);
    backend->registerJSInvoker(jsInvoker);
  }

  void TearDown() override {
    backend.reset();
    uiManager->stopSurface(kSurface);
    uiManager->setDelegate(nullptr);
    ReactNativeFeatureFlags::dangerouslyReset();
  }

  void write(AnimationMutations mutations) {
    backend->pushAnimationMutations(
        [&](AnimationTimestamp /*timestamp*/) { return std::move(mutations); });
  }

  void write(const folly::dynamic& rawProps, bool hasLayoutUpdates = false) {
    AnimationMutations mutations;
    mutations.batch.push_back(
        AnimationMutation{
            .tag = kView,
            .family = view->getFamilyShared(),
            .props =
                {.props = {}, .rawProps = std::make_unique<RawProps>(rawProps)},
            .hasLayoutUpdates = hasLayoutUpdates});
    write(std::move(mutations));
  }

  AnimationMutation mutation(AnimatedProps props) const {
    return AnimationMutation{
        .tag = kView,
        .family = view->getFamilyShared(),
        .props = std::move(props)};
  }

  void write(AnimatedProps props, bool hasLayoutUpdates = false) {
    AnimationMutations mutations;
    mutations.batch.push_back(mutation(std::move(props)));
    mutations.batch.back().hasLayoutUpdates = hasLayoutUpdates;
    write(std::move(mutations));
  }

  static AnimatedProps opacity(Float value) {
    AnimatedPropsBuilder builder;
    builder.setOpacity(value);
    return builder.get();
  }

  void commit(const folly::dynamic& rawProps, ShadowTreeCommitSource source) {
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
        {.source = source});
  }

  void reactCommit(const folly::dynamic& rawProps) {
    commit(rawProps, ShadowTreeCommitSource::React);
  }

  void commitRootChildren(
      const std::vector<std::shared_ptr<const ShadowNode>>& children) {
    shadowTree->commit(
        [&](const RootShadowNode& oldRootShadowNode) {
          return std::static_pointer_cast<RootShadowNode>(
              oldRootShadowNode.ShadowNode::clone(
                  {.children = std::make_shared<
                       std::vector<std::shared_ptr<const ShadowNode>>>(
                       children)}));
        },
        {.source = ShadowTreeCommitSource::React});
  }

  // Writes the final values of animations that finished, to be committed by
  // an end sync.
  void writeFinished(const folly::dynamic& rawProps) {
    AnimationMutations mutations;
    std::unordered_set<std::string> finishedKeys;
    for (const auto& [key, _] : rawProps.items()) {
      finishedKeys.insert(key.getString());
    }
    mutations.batch.push_back(
        AnimationMutation{
            .tag = kView,
            .family = view->getFamilyShared(),
            .props =
                {.props = {}, .rawProps = std::make_unique<RawProps>(rawProps)},
            .finishedKeys = std::move(finishedKeys)});
    mutations.asyncFlushSurfaces.insert(kSurface);
    write(std::move(mutations));
  }

  static void accumulatePropsUpdates() {
    ReactNativeFeatureFlags::dangerouslyReset();
    ReactNativeFeatureFlags::override(
        std::make_unique<AccumulatedPropsFlags>());
  }

  // The view's opacity once the transaction is mounted.
  float mountedOpacity(const MountingTransaction& transaction) const {
    auto props = updatedProps(transaction);
    EXPECT_NE(props, nullptr);
    auto opacity = static_cast<const ViewProps&>(*props).opacity;
#ifdef ANDROID
    // Android judges direct writes when it executes mount batches.
    const auto& telemetry = transaction.getTelemetry();
    auto newer = backend
                     ->newerDirectWrites(
                         kSurface,
                         {{ kView,
                            telemetry.getAnimationStampNumber(),
                            telemetry.getReleaseCoverage() }})
                     .at(0);
    if (newer.count("opacity") != 0) {
      opacity = static_cast<Float>(newer["opacity"].asDouble());
    }
#endif
    return opacity;
  }

  // Releases the node in a frame, like a C++ Animated operation.
  void release(Tag propsNodeTag) {
    backend->release(propsNodeTag);
    write(AnimationMutations{});
  }

  const ViewProps& treeProps() const {
    const auto& root = *shadowTree->getCurrentRevision().rootShadowNode;
    auto ancestors = view->getFamily().getAncestors(root);
    const auto& [parent, index] = ancestors.back();
    return static_cast<const ViewProps&>(
        *parent.get().getChildren().at(index)->getProps());
  }

  MountingTransaction pull() const {
    return *shadowTree->getMountingCoordinator()->pullTransaction();
  }

  static Props::Shared updatedProps(const MountingTransaction& transaction) {
    for (const auto& mutation : transaction.getMutations()) {
      if (mutation.type == ShadowViewMutation::Update &&
          mutation.newChildShadowView.tag == kView) {
        return mutation.newChildShadowView.props;
      }
    }
    return nullptr;
  }

  Props::Shared pulledProps() const {
    return updatedProps(pull());
  }
};

TEST_F(AnimationBackendSurfaceTest, aTransactionCarriesItsRevisionsStamp) {
  write(folly::dynamic::object("opacity", 0.4));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  auto stamped = pull();
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "b"));
  commit(
      folly::dynamic::object("nativeID", "c"), ShadowTreeCommitSource::Unknown);

  EXPECT_EQ(stamped.getTelemetry().getAnimationStampNumber(), 1u);
  EXPECT_EQ(pull().getTelemetry().getAnimationStampNumber(), 2u);
}

TEST_F(AnimationBackendSurfaceTest, mergedTransactionsKeepTheirStamps) {
  write(folly::dynamic::object("opacity", 0.4));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  auto transaction = pull();
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "b"));
  auto mutationCount = transaction.getMutations().size();

  transaction.mergeWith(pull());

  ASSERT_EQ(transaction.getAnimationStamps().size(), 2u);
  EXPECT_EQ(transaction.getAnimationStamps()[0].number, 1u);
  EXPECT_EQ(transaction.getAnimationStamps()[1].number, 2u);
  EXPECT_EQ(transaction.getAnimationStamps()[1].firstMutation, mutationCount);
}

TEST_F(AnimationBackendSurfaceTest, aNewerDirectWriteWinsOverAnOlderRevision) {
  write(folly::dynamic::object("opacity", 0.4));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  write(folly::dynamic::object("opacity", 0.5));

  auto transaction = pull();
  auto props = updatedProps(transaction);

  ASSERT_NE(props, nullptr);
  EXPECT_EQ(props->nativeId, "a");
#ifdef ANDROID
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.4f);
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView, transaction.getTelemetry().getAnimationStampNumber()}}),
      std::vector<folly::dynamic>{folly::dynamic::object("opacity", 0.5)});
#else
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.5f);
#endif
}

TEST_F(AnimationBackendSurfaceTest, aRevisionCoveringTheWriteMountsItsProps) {
  write(folly::dynamic::object("opacity", 0.4));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  write(folly::dynamic::object("opacity", 0.5));
  pull();
  reactCommit(folly::dynamic::object("nativeID", "b"));

  auto transaction = pull();
  auto props = updatedProps(transaction);

  ASSERT_NE(props, nullptr);
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.5f);
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView, transaction.getTelemetry().getAnimationStampNumber()}}),
      std::vector<folly::dynamic>{folly::dynamic::object()});
}

TEST_F(AnimationBackendSurfaceTest, aCommittedWriteIsInItsLayoutRevision) {
  write(folly::dynamic::object("opacity", 0.3));
  write(folly::dynamic::object("opacity", 0.6), true);

  auto transaction = pull();
  auto props = updatedProps(transaction);

  ASSERT_NE(props, nullptr);
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.6f);
}

TEST_F(AnimationBackendSurfaceTest, aPullWithoutRevisionsHasNoTransaction) {
  write(folly::dynamic::object("opacity", 0.5));

  EXPECT_FALSE(shadowTree->getMountingCoordinator()->pullTransaction());
}

TEST_F(AnimationBackendSurfaceTest, aStampCoversTheLatestOfTypedAndRawWrites) {
  write(opacity(0.75));
  write(folly::dynamic::object("opacity", 0.25));
  reactCommit(folly::dynamic::object("nativeID", "a"));

  auto transaction = pull();
  auto props = updatedProps(transaction);

  ASSERT_NE(props, nullptr);
  EXPECT_EQ(transaction.getTelemetry().getAnimationStampNumber(), 2u);
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.25f);
}

TEST_F(AnimationBackendSurfaceTest, aRawResetFollowsATypedWrite) {
  write(opacity(0.75));
  write(folly::dynamic::object("opacity", nullptr));
  reactCommit(folly::dynamic::object("nativeID", "a"));

  auto props = pulledProps();

  ASSERT_NE(props, nullptr);
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 1.0f);
}

TEST_F(AnimationBackendSurfaceTest, edgesFromTwoProducersOfAFrameBothWin) {
  reactCommit(folly::dynamic::object("nativeID", "a"));
  AnimationMutations mutations;
  for (auto colors :
       {CascadedBorderColors{.left = colorFromRGBA(255, 0, 0, 255)},
        CascadedBorderColors{.right = colorFromRGBA(0, 0, 255, 255)}}) {
    AnimatedPropsBuilder builder;
    builder.setBorderColor(colors);
    mutations.batch.push_back(mutation(builder.get()));
  }
  write(std::move(mutations));

  auto transaction = pull();
  auto newer = backend->newerDirectWrites(
      kSurface,
      {{kView, transaction.getTelemetry().getAnimationStampNumber()}});

  ASSERT_EQ(newer.size(), 1u);
  EXPECT_TRUE(newer[0].count("borderLeftColor"));
  EXPECT_TRUE(newer[0].count("borderRightColor"));
#ifndef ANDROID
  auto props = updatedProps(transaction);
  ASSERT_NE(props, nullptr);
  const auto& borderColors = static_cast<const ViewProps&>(*props).borderColors;
  EXPECT_EQ(borderColors.left, colorFromRGBA(255, 0, 0, 255));
  EXPECT_EQ(borderColors.right, colorFromRGBA(0, 0, 255, 255));
#endif
}

TEST_F(AnimationBackendSurfaceTest, anUnrelatedRawWriteKeepsANewerTypedValue) {
  for (bool commitEachWrite : {false, true}) {
    write(folly::dynamic::object("opacity", 0.25));
    if (commitEachWrite) {
      reactCommit(folly::dynamic::object("nativeID", "a"));
    }
    write(opacity(0.75));
    if (commitEachWrite) {
      reactCommit(folly::dynamic::object("nativeID", "b"));
    }
    write(folly::dynamic::object("backgroundColor", 0xff0000ff));
    reactCommit(folly::dynamic::object("nativeID", "c"));

    EXPECT_EQ(treeProps().opacity, 0.75f);
    reactCommit(folly::dynamic::object("nativeID", "d"));
  }
}

TEST_F(AnimationBackendSurfaceTest, aNewerRawWidthOutlivesATypedWidth) {
  AnimatedPropsBuilder builder;
  builder.setWidth(yoga::StyleSizeLength::points(100));
  write(builder.get(), true);
  write(folly::dynamic::object("width", 200), true);

  reactCommit(folly::dynamic::object("nativeID", "a"));

  EXPECT_EQ(
      treeProps().yogaStyle.dimension(yoga::Dimension::Width),
      yoga::StyleSizeLength::points(200));
  auto layoutMetrics =
      std::static_pointer_cast<const LayoutableShadowNode>(
          shadowTree->getCurrentRevision().rootShadowNode->getChildren().at(0))
          ->getLayoutMetrics();
  EXPECT_EQ(layoutMetrics.frame.size.width, 200);
}

TEST_F(AnimationBackendSurfaceTest, aTypedOriginOutlivesUnrelatedRawWrites) {
  TransformOrigin origin;
  origin.xy = {
      ValueUnit(10, UnitType::Point), ValueUnit(50, UnitType::Percent)};
  AnimatedPropsBuilder builder;
  builder.setTransformOrigin(origin);
  write(builder.get());
  reactCommit(folly::dynamic::object("nativeID", "a"));
  write(folly::dynamic::object("opacity", 0.5));

  reactCommit(folly::dynamic::object("nativeID", "b"));

  EXPECT_EQ(treeProps().transformOrigin, origin);
  EXPECT_EQ(
      backend->newerDirectWrites(kSurface, {{kView, 0}})
          .at(0)["transformOrigin"],
      folly::dynamic::array(10.0, "50%", 0.0));
}

TEST_F(AnimationBackendSurfaceTest, aLayoutCommitKeepsEdgesItDoesNotSet) {
  CascadedBorderColors left{.left = colorFromRGBA(255, 0, 0, 255)};
  AnimatedPropsBuilder leftBuilder;
  leftBuilder.setBorderColor(left);
  CascadedBorderRadii topLeft{.topLeft = ValueUnit(5, UnitType::Point)};
  leftBuilder.setBorderRadii(topLeft);
  write(leftBuilder.get());
  reactCommit(folly::dynamic::object("nativeID", "a"));
  CascadedBorderColors right{.right = colorFromRGBA(0, 0, 255, 255)};
  CascadedBorderRadii bottomRight{
      .bottomRight = ValueUnit(50, UnitType::Percent)};
  AnimatedPropsBuilder rightBuilder;
  rightBuilder.setWidth(yoga::StyleSizeLength::points(100));
  rightBuilder.setBorderColor(right);
  rightBuilder.setBorderRadii(bottomRight);

  write(rightBuilder.get(), true);

  const auto& props = treeProps();
  EXPECT_EQ(props.borderColors.left, colorFromRGBA(255, 0, 0, 255));
  EXPECT_EQ(props.borderColors.right, colorFromRGBA(0, 0, 255, 255));
  EXPECT_EQ(props.borderRadii.topLeft, ValueUnit(5, UnitType::Point));
  EXPECT_EQ(props.borderRadii.bottomRight, ValueUnit(50, UnitType::Percent));
}

TEST_F(AnimationBackendSurfaceTest, aTypedEdgeKeepsANewerRawEdge) {
  for (bool commitEachWrite : {false, true}) {
    auto writeMargin = [&](bool left, float points) {
      CascadedRectangleEdges<yoga::StyleLength> margins;
      (left ? margins.left : margins.right) = yoga::StyleLength::points(points);
      AnimatedPropsBuilder builder;
      builder.setMargin(margins);
      write(builder.get(), true);
      if (commitEachWrite) {
        reactCommit(folly::dynamic::object("nativeID", "a"));
      }
    };
    writeMargin(true, 10);
    write(folly::dynamic::object("marginLeft", 20), true);
    writeMargin(false, 30);

    reactCommit(folly::dynamic::object("nativeID", "b"));

    EXPECT_EQ(
        treeProps().yogaStyle.margin(yoga::Edge::Left),
        yoga::StyleLength::points(20));
    EXPECT_EQ(
        treeProps().yogaStyle.margin(yoga::Edge::Right),
        yoga::StyleLength::points(30));
  }
}

TEST_F(AnimationBackendSurfaceTest, aLayoutCommitSetsEveryEdgeOfABorder) {
  AnimatedPropsBuilder builder;
  builder.setWidth(yoga::StyleSizeLength::points(100));
  CascadedBorderColors colors{
      .horizontal = colorFromRGBA(255, 0, 0, 255), .blockStart = SharedColor{}};
  builder.setBorderColor(colors);
  CascadedBorderCurves curves{.topStart = BorderCurve::Continuous};
  builder.setBorderCurves(curves);

  write(builder.get(), true);

  EXPECT_EQ(treeProps().borderColors.horizontal, colorFromRGBA(255, 0, 0, 255));
  EXPECT_EQ(treeProps().borderCurves.topStart, BorderCurve::Continuous);
  auto newer = backend->newerDirectWrites(kSurface, {{kView, 0}}).at(0);
  EXPECT_EQ(newer["borderBlockStartColor"], nullptr);
  EXPECT_EQ(newer["borderTopStartCurve"], "continuous");
}

TEST_F(AnimationBackendSurfaceTest, transformOriginsKeepTheirCenters) {
  AnimatedPropsBuilder unset;
  unset.setTransformOrigin(TransformOrigin{});
  write(unset.get());
  EXPECT_EQ(
      backend->newerDirectWrites(kSurface, {{kView, 0}})
          .at(0)["transformOrigin"],
      nullptr);

  TransformOrigin origin;
  origin.xy = {
      ValueUnit(0, UnitType::Undefined), ValueUnit(20, UnitType::Point)};
  AnimatedPropsBuilder mixed;
  mixed.setTransformOrigin(origin);
  write(mixed.get());

  EXPECT_EQ(
      backend->newerDirectWrites(kSurface, {{kView, 0}})
          .at(0)["transformOrigin"],
      folly::dynamic::array("50%", 20.0, 0.0));
}

namespace {

struct LayoutEdgeProp {
  const char* leftKey;
  void (AnimatedPropsBuilder::*set)(CascadedRectangleEdges<yoga::StyleLength>&);
  yoga::Style::Length (yoga::Style::*get)(yoga::Edge) const;
};

const std::array<LayoutEdgeProp, 4> kLayoutEdgeProps{{
    {"marginLeft", &AnimatedPropsBuilder::setMargin, &yoga::Style::margin},
    {"paddingLeft", &AnimatedPropsBuilder::setPadding, &yoga::Style::padding},
    {"left", &AnimatedPropsBuilder::setPosition, &yoga::Style::position},
    {"borderLeftWidth",
     &AnimatedPropsBuilder::setBorderWidth,
     &yoga::Style::border},
}};

} // namespace

TEST_F(AnimationBackendSurfaceTest, anEmptyTypedEdgeAggregateChangesNoEdge) {
  for (const auto& prop : kLayoutEdgeProps) {
    auto leftEdge = [&] {
      return (treeProps().yogaStyle.*prop.get)(yoga::Edge::Left);
    };
    write(folly::dynamic::object(prop.leftKey, 20), true);
    reactCommit(folly::dynamic::object("nativeID", "a"));
    AnimatedPropsBuilder builder;
    CascadedRectangleEdges<yoga::StyleLength> noEdges;
    (builder.*prop.set)(noEdges);
    write(builder.get(), true);
    reactCommit(folly::dynamic::object("nativeID", "b"));
    EXPECT_EQ(leftEdge(), yoga::StyleLength::points(20)) << prop.leftKey;

    write(folly::dynamic::object(prop.leftKey, 30), true);
    reactCommit(folly::dynamic::object("nativeID", "c"));

    EXPECT_EQ(leftEdge(), yoga::StyleLength::points(30)) << prop.leftKey;
  }
}

TEST_F(AnimationBackendSurfaceTest, anUndefinedTypedEdgeResetsTheEdge) {
  for (const auto& prop : kLayoutEdgeProps) {
    write(folly::dynamic::object(prop.leftKey, 20), true);
    reactCommit(folly::dynamic::object("nativeID", "a"));
    AnimatedPropsBuilder builder;
    CascadedRectangleEdges<yoga::StyleLength> undefinedLeft{
        .left = yoga::StyleLength::undefined()};
    (builder.*prop.set)(undefinedLeft);

    write(builder.get(), true);
    reactCommit(folly::dynamic::object("nativeID", "b"));

    EXPECT_EQ(
        (treeProps().yogaStyle.*prop.get)(yoga::Edge::Left),
        yoga::StyleLength::undefined())
        << prop.leftKey;
  }
}

namespace {

// Drops the transaction it gets, as a delegate with no frame to add does.
class DroppingDelegate : public MountingOverrideDelegate {
 public:
  bool shouldOverridePullTransaction() const override {
    return true;
  }

  std::optional<MountingTransaction> pullTransaction(
      SurfaceId /*surfaceId*/,
      MountingTransaction::Number /*number*/,
      const TransactionTelemetry& /*telemetry*/,
      ShadowViewMutationList /*mutations*/) const override {
    return std::nullopt;
  }
};

// Produces a transaction of its own, as a delegate animating a frame does.
class ProducingDelegate : public MountingOverrideDelegate {
 public:
  bool shouldOverridePullTransaction() const override {
    return true;
  }

  std::optional<MountingTransaction> pullTransaction(
      SurfaceId surfaceId,
      MountingTransaction::Number number,
      const TransactionTelemetry& telemetry,
      ShadowViewMutationList mutations) const override {
    return MountingTransaction{
        surfaceId, number, std::move(mutations), telemetry};
  }
};

} // namespace

namespace {

// Adds a frame to every transaction, as a delegate animating a view does.
class FrameDelegate : public MountingOverrideDelegate {
 public:
  explicit FrameDelegate(ShadowViewMutation frame) : frame_(std::move(frame)) {}

  bool shouldOverridePullTransaction() const override {
    return true;
  }

  std::optional<MountingTransaction> pullTransaction(
      SurfaceId surfaceId,
      MountingTransaction::Number number,
      const TransactionTelemetry& telemetry,
      ShadowViewMutationList mutations) const override {
    mutations.push_back(frame_);
    return MountingTransaction{
        surfaceId, number, std::move(mutations), telemetry};
  }

 private:
  ShadowViewMutation frame_;
};

} // namespace

TEST_F(
    AnimationBackendSurfaceTest,
    aDelegateFrameShowsItsValueUntilANewerDirectWrite) {
  write(folly::dynamic::object("opacity", 0.4));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  const auto& node =
      *shadowTree->getCurrentRevision().rootShadowNode->getChildren().at(0);
  auto frameView = ShadowView(node);
  PropsParserContext context{kSurface, *contextContainer};
  frameView.props = node.getComponentDescriptor().cloneProps(
      context,
      node.getProps(),
      RawProps(folly::dynamic::object("opacity", 0.2)));
  auto dropping = std::make_shared<DroppingDelegate>();
  auto frame =
      std::make_shared<FrameDelegate>(ShadowViewMutation::UpdateMutation(
          ShadowView(node), frameView, kSurface));
  auto coordinator = shadowTree->getMountingCoordinator();
  coordinator->setMountingOverrideDelegate(dropping);
  coordinator->setMountingOverrideDelegate(frame);
  // The opacity the view shows once the transaction is mounted.
  auto mountedOpacity = [&]() {
    auto transaction = coordinator->pullTransaction();
    EXPECT_TRUE(transaction.has_value());
    const auto& telemetry = transaction->getTelemetry();
    auto newer = backend->newerDirectWrites(
        kSurface,
        {{kView,
          telemetry.getAnimationStampNumber(),
          telemetry.getReleaseCoverage()}});
    auto props = updatedProps(*transaction);
    auto opacity = static_cast<const ViewProps&>(*props).opacity;
#ifdef ANDROID
    // Android judges direct writes when it executes mount batches.
    if (!newer.empty() && newer.at(0).count("opacity") != 0) {
      opacity = static_cast<Float>(newer.at(0)["opacity"].asDouble());
    }
#endif
    return opacity;
  };

  EXPECT_EQ(mountedOpacity(), 0.2f);
  EXPECT_EQ(mountedOpacity(), 0.2f);
  write(folly::dynamic::object("opacity", 0.6));
  EXPECT_FLOAT_EQ(mountedOpacity(), 0.6f);
}

TEST_F(
    AnimationBackendSurfaceTest,
    aDelegateFrameAfterADroppedTransactionHasItsRevisionsStamp) {
  auto dropping = std::make_shared<DroppingDelegate>();
  auto producing = std::make_shared<ProducingDelegate>();
  auto coordinator = shadowTree->getMountingCoordinator();
  coordinator->setMountingOverrideDelegate(dropping);
  coordinator->setMountingOverrideDelegate(producing);
  write(folly::dynamic::object("opacity", 0.4));
  reactCommit(folly::dynamic::object("nativeID", "a"));

  auto transaction = coordinator->pullTransaction();

  ASSERT_TRUE(transaction.has_value());
  EXPECT_EQ(transaction->getTelemetry().getAnimationStampNumber(), 1u);
  EXPECT_EQ(transaction->getAnimationStamps().back().number, 1u);
}

constexpr Tag kPropsNode = 100;

TEST_F(AnimationBackendSurfaceTest, ownedWritesStayOutOfReactCommits) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));

  reactCommit(folly::dynamic::object("nativeID", "a"));

  EXPECT_EQ(treeProps().opacity, 0.8f);
  auto transaction = pull();
  const auto& telemetry = transaction.getTelemetry();
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView,
            telemetry.getAnimationStampNumber(),
            telemetry.getReleaseCoverage()}}),
      std::vector<folly::dynamic>{folly::dynamic::object("opacity", 0.5)});
#ifndef ANDROID
  auto props = updatedProps(transaction);
  ASSERT_NE(props, nullptr);
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.5f);
#endif
}

TEST_F(AnimationBackendSurfaceTest, ownedPropsStayOutOfLayoutCommits) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});

  write(folly::dynamic::object("opacity", 0.5)("width", 50), true);

  EXPECT_EQ(treeProps().opacity, 0.8f);
  EXPECT_EQ(
      treeProps().yogaStyle.dimension(yoga::Dimension::Width),
      yoga::StyleSizeLength::points(50));
}

TEST_F(AnimationBackendSurfaceTest, aReleaseMountsReactsValue) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();

  release(kPropsNode);

  auto transaction = pull();
  const auto& telemetry = transaction.getTelemetry();
  auto releaseCoverage = telemetry.getReleaseCoverage();
  EXPECT_GT(releaseCoverage, 0u);
  auto props = updatedProps(transaction);
  ASSERT_NE(props, nullptr);
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.8f);
  auto stamp = telemetry.getAnimationStampNumber();
  EXPECT_EQ(
      backend->newerDirectWrites(kSurface, {{kView, stamp, releaseCoverage}}),
      std::vector<folly::dynamic>{folly::dynamic::object()});
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface, {{kView, stamp, releaseCoverage - 1}}),
      std::vector<folly::dynamic>{folly::dynamic::object("opacity", 0.5)});
#ifdef RN_SERIALIZABLE_STATE
  auto releasedValues =
      backend->releasedValues(kSurface, kView, 0, releaseCoverage, props);
  EXPECT_NEAR(releasedValues["opacity"].asDouble(), 0.8, 0.0001);
#endif
}

TEST_F(AnimationBackendSurfaceTest, aReleasedViewWithoutAnUpdateIsUpdated) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();
  auto mountedRoot = shadowTree->getCurrentRevision().rootShadowNode;
  release(kPropsNode);

  shadowTree->commit(
      [&](const RootShadowNode& /*oldRootShadowNode*/) {
        return std::static_pointer_cast<RootShadowNode>(
            mountedRoot->ShadowNode::clone({}));
      },
      {});

  auto transaction = pull();
  auto props = updatedProps(transaction);
  ASSERT_NE(props, nullptr);
  EXPECT_NE(props, mountedRoot->getChildren().at(0)->getProps());
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.8f);
}

TEST_F(AnimationBackendSurfaceTest, aFinishedOwnedValueReachesTheTree) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});

  writeFinished(folly::dynamic::object("opacity", 0.3));

  EXPECT_EQ(treeProps().opacity, 0.3f);
}

TEST_F(AnimationBackendSurfaceTest, aReleaseKeepsKeysANewerNodeOwns) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  backend->own(view->getFamilyShared(), kPropsNode + 1, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));

  release(kPropsNode);

  reactCommit(folly::dynamic::object("nativeID", "a"));
  auto transaction = pull();
  EXPECT_EQ(transaction.getTelemetry().getReleaseCoverage(), 0u);
  const auto& telemetry = transaction.getTelemetry();
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView,
            telemetry.getAnimationStampNumber(),
            telemetry.getReleaseCoverage()}}),
      std::vector<folly::dynamic>{folly::dynamic::object("opacity", 0.5)});
}

TEST_F(AnimationBackendSurfaceTest, aLayoutUpdateOfAReleasedViewAppliesProps) {
  accumulatePropsUpdates();
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();
  auto mountedRoot = shadowTree->getCurrentRevision().rootShadowNode;
  release(kPropsNode);

  shadowTree->commit(
      [&](const RootShadowNode& /*oldRootShadowNode*/) {
        return mountedRoot->clone(
            PropsParserContext{kSurface, *contextContainer},
            LayoutConstraints{.minimumSize = {50, 50}, .maximumSize = {50, 50}},
            LayoutContext{});
      },
      {});

  auto transaction = pull();
  const ShadowViewMutation* update = nullptr;
  for (const auto& mutation : transaction.getMutations()) {
    if (mutation.type == ShadowViewMutation::Update &&
        mutation.newChildShadowView.tag == kView) {
      update = &mutation;
    }
  }
  ASSERT_NE(update, nullptr);
  EXPECT_NE(
      update->oldChildShadowView.layoutMetrics,
      update->newChildShadowView.layoutMetrics);
  EXPECT_NE(update->newChildShadowView.props, update->oldChildShadowView.props);
  EXPECT_EQ(
      static_cast<const ViewProps&>(*update->newChildShadowView.props).opacity,
      0.8f);
}

TEST_F(AnimationBackendSurfaceTest, aPullAfterAReleaseAndLaterCommitsMountsIt) {
  accumulatePropsUpdates();
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();
  release(kPropsNode);
  reactCommit(folly::dynamic::object("nativeID", "b"));
  reactCommit(folly::dynamic::object("nativeID", "c"));

  auto transaction = pull();

  const auto& telemetry = transaction.getTelemetry();
  EXPECT_GT(telemetry.getReleaseCoverage(), 0u);
  auto props = updatedProps(transaction);
  ASSERT_NE(props, nullptr);
  EXPECT_EQ(props->nativeId, "c");
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.8f);
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView,
            telemetry.getAnimationStampNumber(),
            telemetry.getReleaseCoverage()}}),
      std::vector<folly::dynamic>{folly::dynamic::object()});
}

TEST_F(AnimationBackendSurfaceTest, aReconnectionBeforeTheReleaseKeepsOwning) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  jsInvoker->defers = true;
  backend->release(kPropsNode);
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.7));

  jsInvoker->runDeferredTasks();

  reactCommit(folly::dynamic::object("nativeID", "a"));
  auto transaction = pull();
  const auto& telemetry = transaction.getTelemetry();
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView,
            telemetry.getAnimationStampNumber(),
            telemetry.getReleaseCoverage()}}),
      std::vector<folly::dynamic>{folly::dynamic::object("opacity", 0.7)});
}

namespace {

class CommitBranchingFlags : public ReactNativeFeatureFlagsDefaults {
 public:
  bool enableFabricCommitBranching() override {
    return true;
  }
};

} // namespace

TEST_F(
    AnimationBackendSurfaceTest,
    aMergedReactRevisionFromBeforeAReleaseKeepsTheOwner) {
  ReactNativeFeatureFlags::dangerouslyReset();
  ReactNativeFeatureFlags::override(std::make_unique<CommitBranchingFlags>());
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  shadowTree->commit(
      [&](const RootShadowNode& oldRootShadowNode) {
        return std::static_pointer_cast<RootShadowNode>(
            oldRootShadowNode.ShadowNode::clone({}));
      },
      {.mountSynchronously = false, .source = ShadowTreeCommitSource::React});
  shadowTree->promoteReactRevision();
  release(kPropsNode);

  shadowTree->mergeReactRevision();
  auto beforeRelease = pull();
  shadowTree->promoteReactRevision();
  shadowTree->mergeReactRevision();
  auto afterRelease = pull();

  auto newerDirectWrites = [&](const MountingTransaction& transaction) {
    const auto& telemetry = transaction.getTelemetry();
    return backend
        ->newerDirectWrites(
            kSurface,
            {{kView,
              telemetry.getAnimationStampNumber(),
              telemetry.getReleaseCoverage()}})
        .at(0);
  };
  EXPECT_EQ(beforeRelease.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_EQ(
      newerDirectWrites(beforeRelease),
      folly::dynamic(folly::dynamic::object("opacity", 0.5)));
  EXPECT_GT(afterRelease.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_EQ(
      newerDirectWrites(afterRelease),
      folly::dynamic(folly::dynamic::object()));
}

TEST_F(
    AnimationBackendSurfaceTest,
    aReleaseAfterAQueuedEndSyncIsCommittedAfterIt) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  backend->own(view->getFamilyShared(), kPropsNode + 1, {"shadowOpacity"});
  jsInvoker->defers = true;
  release(kPropsNode + 1);
  writeFinished(folly::dynamic::object("opacity", 0.5));
  release(kPropsNode);

  jsInvoker->runDeferredTasks();

  EXPECT_EQ(treeProps().opacity, 0.5f);
}

namespace {

class EndSyncClearingFlags : public ReactNativeFeatureFlagsDefaults {
 public:
  bool updateRuntimeShadowNodeReferencesOnCommitThread() override {
    return true;
  }
};

// Runs a callback in the next end sync commit, after the hooks registered
// before it.
class EndSyncHook : public UIManagerCommitHook {
 public:
  explicit EndSyncHook(std::function<void()> callback)
      : callback_(std::move(callback)) {}

  void commitHookWasRegistered(
      const UIManager& /*uiManager*/) noexcept override {}
  void commitHookWasUnregistered(
      const UIManager& /*uiManager*/) noexcept override {}

  RootShadowNode::Unshared shadowTreeWillCommit(
      const ShadowTree& /*shadowTree*/,
      const RootShadowNode::Shared& /*oldRootShadowNode*/,
      const RootShadowNode::Unshared& newRootShadowNode,
      const ShadowTreeCommitOptions& commitOptions) noexcept override {
    if (commitOptions.source == ShadowTreeCommitSource::AnimationEndSync &&
        callback_) {
      std::exchange(callback_, nullptr)();
    }
    return newRootShadowNode;
  }

 private:
  std::function<void()> callback_;
};

} // namespace

TEST_F(AnimationBackendSurfaceTest, anEndSyncKeepsValuesFinishedDuringIt) {
  ReactNativeFeatureFlags::dangerouslyReset();
  ReactNativeFeatureFlags::override(std::make_unique<EndSyncClearingFlags>());
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  backend->own(view->getFamilyShared(), kPropsNode + 1, {"shadowOpacity"});
  EndSyncHook hook([&] {
    jsInvoker->defers = true;
    writeFinished(folly::dynamic::object("shadowOpacity", 0.3));
  });
  uiManager->registerCommitHook(hook);
  jsInvoker->defers = true;
  writeFinished(folly::dynamic::object("opacity", 0.5));

  jsInvoker->runDeferredTasks();
  jsInvoker->runDeferredTasks();

  uiManager->unregisterCommitHook(hook);
  EXPECT_EQ(treeProps().opacity, 0.5f);
  EXPECT_EQ(treeProps().shadowOpacity, 0.3f);
}

TEST_F(
    AnimationBackendSurfaceTest,
    aTypedValueWrittenBeforeOwnershipIsNotStamped) {
  write(opacity(0.4));
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.9));

  reactCommit(folly::dynamic::object("opacity", 0.2));

  EXPECT_EQ(treeProps().opacity, 0.2f);
}

TEST_F(AnimationBackendSurfaceTest, ownedTypedPropsStayOutOfCommits) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  AnimatedPropsBuilder propsBuilder;
  propsBuilder.setOpacity(0.5);
  propsBuilder.setWidth(yoga::StyleSizeLength::points(50));

  write(propsBuilder.get(), true);
  reactCommit(folly::dynamic::object("nativeID", "a"));

  EXPECT_EQ(treeProps().opacity, 0.8f);
  EXPECT_EQ(
      treeProps().yogaStyle.dimension(yoga::Dimension::Width),
      yoga::StyleSizeLength::points(50));
}

TEST_F(
    AnimationBackendSurfaceTest,
    aReconnectionDropsTheFinalValueOfThePreviousConnection) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  jsInvoker->defers = true;
  writeFinished(folly::dynamic::object("opacity", 0.9));
  backend->release(kPropsNode);
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.4));

  jsInvoker->runDeferredTasks();

  EXPECT_EQ(treeProps().opacity, 0.8f);
  reactCommit(folly::dynamic::object("nativeID", "a"));
  auto transaction = pull();
  const auto& telemetry = transaction.getTelemetry();
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView,
            telemetry.getAnimationStampNumber(),
            telemetry.getReleaseCoverage()}}),
      std::vector<folly::dynamic>{folly::dynamic::object("opacity", 0.4)});
}

TEST_F(AnimationBackendSurfaceTest, aStaleReleaseKeepsNewerWrites) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  backend->own(view->getFamilyShared(), kPropsNode + 1, {"opacity"});
  release(kPropsNode + 1);
  write(folly::dynamic::object("opacity", 0.7));

  release(kPropsNode);
  reactCommit(folly::dynamic::object("nativeID", "a"));

  EXPECT_EQ(treeProps().opacity, 0.7f);
}

namespace {

// Connects a props node that animates `key` of `view` through a style node.
void connectPropsNode(
    NativeAnimatedNodesManager& nodesManager,
    const std::shared_ptr<const ShadowNodeFamily>& family,
    Tag valueTag,
    Tag propsNodeTag,
    const std::string& key,
    double value) {
  auto styleTag = valueTag + 1;
  nodesManager.createAnimatedNode(
      valueTag,
      folly::dynamic::object("type", "value")("value", value)("offset", 0));
  nodesManager.createAnimatedNode(
      styleTag,
      folly::dynamic::object("type", "style")(
          "style", folly::dynamic::object(key, valueTag)));
  nodesManager.createAnimatedNode(
      propsNodeTag,
      folly::dynamic::object("type", "props")(
          "props", folly::dynamic::object("style", styleTag)));
  nodesManager.connectAnimatedNodes(valueTag, styleTag);
  nodesManager.connectAnimatedNodes(styleTag, propsNodeTag);
  nodesManager.connectAnimatedNodeToView(propsNodeTag, family->getTag());
  nodesManager.connectAnimatedNodeToShadowNodeFamily(
      propsNodeTag, std::const_pointer_cast<ShadowNodeFamily>(family));
}

void startFramesAnimation(
    NativeAnimatedNodesManager& nodesManager,
    int animationId,
    Tag valueTag,
    size_t frameCount,
    double toValue) {
  auto frames = folly::dynamic::array();
  for (size_t i = 0; i < frameCount; i++) {
    frames.push_back(static_cast<double>(i) / (frameCount - 1));
  }
  nodesManager.startAnimatingNode(
      animationId,
      valueTag,
      folly::dynamic::object("type", "frames")("frames", frames)(
          "toValue", toValue),
      std::nullopt);
}

} // namespace

TEST_F(
    AnimationBackendSurfaceTest,
    anEndSyncCommitsOnlyTheFinishedNodesValues) {
  auto nodesManager = std::make_shared<NativeAnimatedNodesManager>(backend);
  nodesManager->scheduleOnUI([&] {
    connectPropsNode(
        *nodesManager, view->getFamilyShared(), 1, kPropsNode, "opacity", 0.5);
    connectPropsNode(
        *nodesManager,
        view->getFamilyShared(),
        3,
        kPropsNode + 1,
        "shadowOpacity",
        0.1);
    startFramesAnimation(*nodesManager, 1, 1, 60, 0.2);
    startFramesAnimation(*nodesManager, 2, 3, 2, 0.4);
  });

  backend->onAnimationFrame(AnimationTimestamp{1000});
  backend->onAnimationFrame(AnimationTimestamp{1100});

  EXPECT_EQ(treeProps().shadowOpacity, 0.4f);
  EXPECT_EQ(treeProps().opacity, 0.8f);
}

TEST_F(
    AnimationBackendSurfaceTest,
    restoringDefaultValuesDropsTheQueuedFinalValue) {
  accumulatePropsUpdates();
  auto nodesManager = std::make_shared<NativeAnimatedNodesManager>(backend);
  nodesManager->scheduleOnUI([&] {
    connectPropsNode(
        *nodesManager, view->getFamilyShared(), 1, kPropsNode, "opacity", 0.5);
    startFramesAnimation(*nodesManager, 1, 1, 2, 0.9);
  });
  jsInvoker->defers = true;
  backend->onAnimationFrame(AnimationTimestamp{1000});
  backend->onAnimationFrame(AnimationTimestamp{1100});
  nodesManager->scheduleOnUI(
      [&] { nodesManager->restoreDefaultValues(kPropsNode); });
  backend->onAnimationFrame(AnimationTimestamp{1200});

  jsInvoker->runDeferredTasks();
  reactCommit(folly::dynamic::object("nativeID", "a"));

  EXPECT_EQ(treeProps().opacity, 0.8f);
  auto props = updatedProps(pull());
  ASSERT_NE(props, nullptr);
  EXPECT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.8f);
}

TEST_F(AnimationBackendSurfaceTest, structuralPropsAreNotOwned) {
  auto nodesManager = std::make_shared<NativeAnimatedNodesManager>(backend);
  nodesManager->scheduleOnUI([&] {
    connectPropsNode(
        *nodesManager, view->getFamilyShared(), 1, kPropsNode, "zIndex", 2);
  });
  backend->onAnimationFrame(AnimationTimestamp{1000});

  reactCommit(folly::dynamic::object("nativeID", "a"));

  EXPECT_EQ(treeProps().zIndex, 2);
}

TEST_F(AnimationBackendSurfaceTest, paragraphColorIsNotOwned) {
  auto paragraph =
      builder.build(Element<ParagraphShadowNode>().tag(30).surfaceId(kSurface));
  auto nodesManager = std::make_shared<NativeAnimatedNodesManager>(backend);
  nodesManager->scheduleOnUI([&] {
    connectPropsNode(
        *nodesManager,
        paragraph->getFamilyShared(),
        1,
        kPropsNode,
        "color",
        4278190335.0);
    connectPropsNode(
        *nodesManager,
        view->getFamilyShared(),
        3,
        kPropsNode + 1,
        "color",
        4278190335.0);
  });
  backend->onAnimationFrame(AnimationTimestamp{1000});

  reactCommit(folly::dynamic::object("nativeID", "a"));

  auto transaction = pull();
  const auto& telemetry = transaction.getTelemetry();
  auto stamp = telemetry.getAnimationStampNumber();
  auto releaseCoverage = telemetry.getReleaseCoverage();
  auto newer = backend->newerDirectWrites(
      kSurface,
      {{30, stamp, releaseCoverage}, {kView, stamp, releaseCoverage}});
  EXPECT_EQ(newer.at(0), folly::dynamic(folly::dynamic::object()));
  EXPECT_EQ(newer.at(1).count("color"), 1u);
}

TEST_F(AnimationBackendSurfaceTest, aReleaseKeepsTheValuesOfADelegateFrame) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();
  const auto& node =
      *shadowTree->getCurrentRevision().rootShadowNode->getChildren().at(0);
  auto frameView = ShadowView(node);
  PropsParserContext context{kSurface, *contextContainer};
  frameView.props = node.getComponentDescriptor().cloneProps(
      context,
      node.getProps(),
      RawProps(folly::dynamic::object("opacity", 0.3)));
  auto dropping = std::make_shared<DroppingDelegate>();
  auto frame = std::make_shared<FrameDelegate>(
      ShadowViewMutation::UpdateMutation(frameView, frameView, kSurface));
  auto coordinator = shadowTree->getMountingCoordinator();
  coordinator->setMountingOverrideDelegate(dropping);
  coordinator->setMountingOverrideDelegate(frame);

  release(kPropsNode);

  auto props = updatedProps(pull());
  ASSERT_NE(props, nullptr);
  EXPECT_NE(props, frameView.props);
  EXPECT_FLOAT_EQ(static_cast<const ViewProps&>(*props).opacity, 0.3f);
}

TEST_F(
    AnimationBackendSurfaceTest,
    mergedGroupsAndDelegatePullsKeepTheirReleaseCoverage) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  auto merged = pull();
  release(kPropsNode);
  merged.mergeWith(pull());
  const auto& node =
      *shadowTree->getCurrentRevision().rootShadowNode->getChildren().at(0);
  auto frame =
      std::make_shared<FrameDelegate>(ShadowViewMutation::UpdateMutation(
          ShadowView(node), ShadowView(node), kSurface));
  shadowTree->getMountingCoordinator()->setMountingOverrideDelegate(frame);

  auto synthetic = shadowTree->getMountingCoordinator()->pullTransaction();

  const auto& stamps = merged.getAnimationStamps();
  ASSERT_EQ(stamps.size(), 2u);
  ASSERT_TRUE(synthetic.has_value());
  const auto& telemetry = synthetic->getTelemetry();
  EXPECT_EQ(telemetry.getReleaseCoverage(), stamps[1].releaseCoverage);
  EXPECT_EQ(
      backend->newerDirectWrites(
          kSurface,
          {{kView, stamps[0].number, stamps[0].releaseCoverage},
           {kView, stamps[1].number, stamps[1].releaseCoverage},
           {kView,
            telemetry.getAnimationStampNumber(),
            telemetry.getReleaseCoverage()}}),
      (std::vector<folly::dynamic>{
          folly::dynamic::object("opacity", 0.5),
          folly::dynamic::object(),
          folly::dynamic::object()}));
}

namespace {

class ViewCullingFlags : public AccumulatedPropsFlags {
 public:
  bool enableViewCulling() override {
    return true;
  }
};

constexpr Tag kScrollView = 20;

} // namespace

// `view` in a 100x100 scroll view, which culls the views outside of it.
class AnimationBackendCullingTest : public AnimationBackendSurfaceTest {
 protected:
  std::shared_ptr<const ShadowNode> buildRootChild() override {
    ReactNativeFeatureFlags::dangerouslyReset();
    ReactNativeFeatureFlags::override(std::make_unique<ViewCullingFlags>());
    return builder.build(
        Element<ScrollViewShadowNode>()
            .tag(kScrollView)
            .surfaceId(kSurface)
            .props([] {
              auto props = std::make_shared<ScrollViewProps>();
              props->yogaStyle.setDimension(
                  yoga::Dimension::Width, yoga::StyleSizeLength::points(100));
              props->yogaStyle.setDimension(
                  yoga::Dimension::Height, yoga::StyleSizeLength::points(100));
              props->yogaStyle.setOverflow(yoga::Overflow::Scroll);
              return props;
            })
            .children({Element<ViewShadowNode>()
                           .tag(kView)
                           .surfaceId(kSurface)
                           .reference(view)
                           .props([] {
                             auto props = std::make_shared<ViewProps>();
                             props->opacity = 0.8;
                             props->yogaStyle.setPositionType(
                                 yoga::PositionType::Absolute);
                             props->yogaStyle.setDimension(
                                 yoga::Dimension::Width,
                                 yoga::StyleSizeLength::points(10));
                             props->yogaStyle.setDimension(
                                 yoga::Dimension::Height,
                                 yoga::StyleSizeLength::points(10));
                             return props;
                           })}));
  }

  static bool deletes(const MountingTransaction& transaction, Tag tag) {
    const auto& mutations = transaction.getMutations();
    return std::any_of(
        mutations.begin(), mutations.end(), [&](const auto& mutation) {
          return mutation.type == ShadowViewMutation::Delete &&
              mutation.oldChildShadowView.tag == tag;
        });
  }

  void moveView(int top) {
    reactCommit(folly::dynamic::object("top", top));
  }
};

TEST_F(AnimationBackendCullingTest, aCulledReleasedViewIsNotUpdated) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  moveView(1000);
  ASSERT_TRUE(deletes(pull(), kView));

  release(kPropsNode);

  auto transaction = pull();
  EXPECT_GT(transaction.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_EQ(updatedProps(transaction), nullptr);
}

TEST_F(
    AnimationBackendCullingTest,
    aReleasedViewCulledInTheSameTransactionIsNotUpdated) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();

  release(kPropsNode);
  moveView(1000);

  auto transaction = pull();
  EXPECT_GT(transaction.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_TRUE(deletes(transaction, kView));
  EXPECT_EQ(updatedProps(transaction), nullptr);
}

TEST_F(AnimationBackendCullingTest, aReleasedViewCreatedAgainShowsReactsValue) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  moveView(1000);
  pull();
  release(kPropsNode);
  pull();

  moveView(0);

  auto transaction = pull();
  const auto& mutations = transaction.getMutations();
  auto create = std::find_if(
      mutations.begin(), mutations.end(), [](const auto& mutation) {
        return mutation.type == ShadowViewMutation::Create &&
            mutation.newChildShadowView.tag == kView;
      });
  ASSERT_NE(create, mutations.end());
  EXPECT_EQ(
      static_cast<const ViewProps&>(*create->newChildShadowView.props).opacity,
      0.8f);
}

TEST_F(AnimationBackendSurfaceTest, anOwnedTypedWriteWinsUntilItsRelease) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(opacity(0.5));

  reactCommit(folly::dynamic::object("opacity", 0.2));
  EXPECT_FLOAT_EQ(mountedOpacity(pull()), 0.5f);
  release(kPropsNode);

  auto transaction = pull();
  EXPECT_GT(transaction.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(transaction), 0.2f);
}

namespace {

// Cancels release commits while `cancels`, after the hooks registered before
// it.
class ReleaseCancellingHook : public UIManagerCommitHook {
 public:
  void commitHookWasRegistered(
      const UIManager& /*uiManager*/) noexcept override {}
  void commitHookWasUnregistered(
      const UIManager& /*uiManager*/) noexcept override {}

  RootShadowNode::Unshared shadowTreeWillCommit(
      const ShadowTree& /*shadowTree*/,
      const RootShadowNode::Shared& /*oldRootShadowNode*/,
      const RootShadowNode::Unshared& newRootShadowNode,
      const ShadowTreeCommitOptions& commitOptions) noexcept override {
    if (commitOptions.source != ShadowTreeCommitSource::AnimatedPropsRelease) {
      return newRootShadowNode;
    }
    attempts++;
    return cancels ? nullptr : newRootShadowNode;
  }

  bool cancels{true};
  int attempts{0};
};

} // namespace

TEST_F(AnimationBackendSurfaceTest, cancelledReleasesAreRetriedOnceAFrame) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  backend->own(view->getFamilyShared(), kPropsNode + 1, {"shadowOpacity"});
  write(folly::dynamic::object("opacity", 0.5)("shadowOpacity", 0.3));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();
  ReleaseCancellingHook hook;
  uiManager->registerCommitHook(hook);
  jsInvoker->defers = true;
  release(kPropsNode);
  jsInvoker->runQueuedTasks();
  backend->onAnimationFrame(AnimationTimestamp{1000});
  release(kPropsNode + 1);
  jsInvoker->runQueuedTasks();
  EXPECT_EQ(jsInvoker->queuedTaskCount(), 0u);

  reactCommit(folly::dynamic::object("opacity", 0.2)("shadowOpacity", 0.1));
  auto cancelled = pull();
  hook.cancels = false;
  backend->onAnimationFrame(AnimationTimestamp{1016});
  backend->onAnimationFrame(AnimationTimestamp{1032});
  EXPECT_EQ(jsInvoker->queuedTaskCount(), 1u);
  jsInvoker->runQueuedTasks();
  auto committed = pull();
  backend->onAnimationFrame(AnimationTimestamp{1048});

  uiManager->unregisterCommitHook(hook);
  EXPECT_EQ(jsInvoker->queuedTaskCount(), 0u);
  EXPECT_EQ(cancelled.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(cancelled), 0.5f);
  EXPECT_GT(committed.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(committed), 0.2f);
  EXPECT_FLOAT_EQ(treeProps().shadowOpacity, 0.1f);
}

TEST_F(
    AnimationBackendSurfaceTest,
    aCancelledReleaseCommittedAgainKeepsANewerOwner) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  ReleaseCancellingHook hook;
  uiManager->registerCommitHook(hook);
  jsInvoker->defers = true;
  release(kPropsNode);
  jsInvoker->runQueuedTasks();
  hook.cancels = false;

  backend->own(view->getFamilyShared(), kPropsNode + 1, {"opacity"});
  write(folly::dynamic::object("opacity", 0.7));
  backend->onAnimationFrame(AnimationTimestamp{1000});
  jsInvoker->runDeferredTasks();

  uiManager->unregisterCommitHook(hook);
  auto transaction = pull();
  EXPECT_GT(transaction.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(transaction), 0.7f);
}

TEST_F(AnimationBackendSurfaceTest, aStoppedSurfaceDropsItsCancelledRelease) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  ReleaseCancellingHook hook;
  uiManager->registerCommitHook(hook);
  jsInvoker->defers = true;
  release(kPropsNode);
  jsInvoker->runQueuedTasks();
  backend->onAnimationFrame(AnimationTimestamp{1000});
  uiManager->unregisterCommitHook(hook);
  std::weak_ptr<const ShadowNodeFamily> family = view->getFamilyShared();

  uiManager->stopSurface(kSurface).reset();
  backend->clearRegistryOnSurfaceStop(kSurface);
  view.reset();

  EXPECT_TRUE(family.expired());
  jsInvoker->runQueuedTasks();
  backend->onAnimationFrame(AnimationTimestamp{1016});
  EXPECT_EQ(jsInvoker->queuedTaskCount(), 0u);
}

TEST_F(AnimationBackendSurfaceTest, aPendingReleaseOfRemovedViewsIsDropped) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  ReleaseCancellingHook hook;
  uiManager->registerCommitHook(hook);
  jsInvoker->defers = true;
  release(kPropsNode);
  jsInvoker->runQueuedTasks();
  std::weak_ptr<const ShadowNodeFamily> family = view->getFamilyShared();

  commitRootChildren({});
  pull();
  view.reset();
  ASSERT_TRUE(family.expired());
  auto attempts = hook.attempts;
  for (int frame = 0; frame < 3; frame++) {
    backend->onAnimationFrame(AnimationTimestamp{1000.0 + 16 * frame});
    jsInvoker->runQueuedTasks();
  }
  backend->onAnimationFrame(AnimationTimestamp{1048});

  uiManager->unregisterCommitHook(hook);
  EXPECT_EQ(hook.attempts, attempts);
  EXPECT_EQ(jsInvoker->queuedTaskCount(), 0u);
}

TEST_F(
    AnimationBackendSurfaceTest,
    aPendingReleaseKeepsItsViewsThatAreStillMounted) {
  auto other = builder.build(
      Element<ViewShadowNode>().tag(kView + 1).surfaceId(kSurface).props([] {
        auto props = std::make_shared<ViewProps>();
        props->opacity = 0.6;
        return props;
      }));
  commitRootChildren({view, other});
  pull();
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  backend->own(other->getFamilyShared(), kPropsNode + 1, {"opacity"});
  AnimationMutations mutations;
  for (const auto& node : {view, other}) {
    mutations.batch.push_back(
        AnimationMutation{
            .tag = node->getTag(),
            .family = node->getFamilyShared(),
            .props = {
                .props = {},
                .rawProps = std::make_unique<RawProps>(
                    folly::dynamic::object("opacity", 0.5))}});
  }
  write(std::move(mutations));
  ReleaseCancellingHook hook;
  uiManager->registerCommitHook(hook);
  jsInvoker->defers = true;
  backend->release(kPropsNode);
  backend->release(kPropsNode + 1);
  write(AnimationMutations{});
  jsInvoker->runQueuedTasks();
  std::weak_ptr<const ShadowNodeFamily> otherFamily = other->getFamilyShared();
  commitRootChildren({view});
  pull();
  other.reset();
  ASSERT_TRUE(otherFamily.expired());

  hook.cancels = false;
  backend->onAnimationFrame(AnimationTimestamp{1000});
  jsInvoker->runQueuedTasks();

  uiManager->unregisterCommitHook(hook);
  auto transaction = pull();
  EXPECT_EQ(hook.attempts, 2);
  EXPECT_GT(transaction.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(transaction), 0.8f);
}

TEST_F(AnimationBackendSurfaceTest, aFinishedValueIsTheOneTheFrameWrote) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  AnimatedPropsBuilder propsBuilder;
  propsBuilder.setOpacity(0.7);
  propsBuilder.setWidth(yoga::StyleSizeLength::points(50));
  auto props = propsBuilder.get();
  props.rawProps =
      std::make_unique<RawProps>(folly::dynamic::object("opacity", 0.3));
  AnimationMutations mutations;
  mutations.batch.push_back(mutation(std::move(props)));
  mutations.batch.back().hasLayoutUpdates = true;
  mutations.batch.back().finishedKeys = {"opacity"};
  mutations.asyncFlushSurfaces.insert(kSurface);

  write(std::move(mutations));
  release(kPropsNode);

  EXPECT_FLOAT_EQ(treeProps().opacity, 0.7f);
  EXPECT_FLOAT_EQ(mountedOpacity(pull()), 0.7f);
}

TEST_F(
    AnimationBackendSurfaceTest,
    aNodeConnectedBeforeTheWritersDisconnectionKeepsTheValue) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  backend->own(view->getFamilyShared(), kPropsNode + 1, {"opacity"});
  release(kPropsNode);

  reactCommit(folly::dynamic::object("opacity", 0.2));
  auto owned = pull();
  release(kPropsNode + 1);
  auto released = pull();

  EXPECT_EQ(owned.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(owned), 0.5f);
  EXPECT_GT(released.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(released), 0.2f);
}

TEST_F(AnimationBackendSurfaceTest, aReconnectedNodeDoesNotKeepTheValue) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(folly::dynamic::object("opacity", 0.5));
  reactCommit(folly::dynamic::object("nativeID", "a"));
  pull();

  backend->release(kPropsNode);
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  write(AnimationMutations{});

  auto transaction = pull();
  EXPECT_GT(transaction.getTelemetry().getReleaseCoverage(), 0u);
  EXPECT_FLOAT_EQ(mountedOpacity(transaction), 0.8f);
}

TEST_F(
    AnimationBackendSurfaceTest,
    anEndSyncCommitsTheValueItTookBeforeANewOwner) {
  backend->own(view->getFamilyShared(), kPropsNode, {"opacity"});
  EndSyncHook hook([&] {
    jsInvoker->defers = true;
    backend->own(view->getFamilyShared(), kPropsNode + 1, {"opacity"});
    write(folly::dynamic::object("opacity", 0.4));
  });
  uiManager->registerCommitHook(hook);
  jsInvoker->defers = true;
  writeFinished(folly::dynamic::object("opacity", 0.9));

  jsInvoker->runDeferredTasks();

  uiManager->unregisterCommitHook(hook);
  EXPECT_FLOAT_EQ(treeProps().opacity, 0.9f);
  reactCommit(folly::dynamic::object("nativeID", "a"));
  EXPECT_FLOAT_EQ(mountedOpacity(pull()), 0.4f);
}

} // namespace facebook::react
