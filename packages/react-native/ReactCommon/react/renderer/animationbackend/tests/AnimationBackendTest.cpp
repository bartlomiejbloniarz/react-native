/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <react/renderer/animationbackend/AnimatedPropsBuilder.h>
#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/animationbackend/AnimationBackend.h>
#include <react/renderer/components/root/RootShadowNode.h>
#include <react/renderer/components/view/ViewShadowNode.h>
#include <react/renderer/element/ComponentBuilder.h>
#include <react/renderer/element/Element.h>
#include <react/renderer/element/testUtils.h>
#include <react/renderer/graphics/Color.h>
#include <react/renderer/uimanager/UIManager.h>
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

class ImmediateCallInvoker : public CallInvoker {
 public:
  void invokeAsync(CallFunc&& /*func*/) noexcept override {}
  void invokeAsync(std::function<void()>&& func) noexcept override {
    func();
  }
  void invokeSync(CallFunc&& /*func*/) override {}
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
    pull();
    backend = std::make_shared<AnimationBackend>(
        std::make_shared<TestAnimationChoreographer>(), uiManager);
    backend->registerJSInvoker(std::make_shared<ImmediateCallInvoker>());
  }

  void TearDown() override {
    backend.reset();
    uiManager->stopSurface(kSurface);
    uiManager->setDelegate(nullptr);
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

  const ViewProps& treeProps() const {
    return static_cast<const ViewProps&>(*shadowTree->getCurrentRevision()
                                              .rootShadowNode->getChildren()
                                              .at(0)
                                              ->getProps());
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

} // namespace facebook::react
