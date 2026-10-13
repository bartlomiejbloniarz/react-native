/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <hermes/hermes.h>
#include <react/renderer/animationbackend/AnimatedPropsBuilder.h>
#include <react/renderer/animationbackend/AnimatedPropsRegistry.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/element/ComponentBuilder.h>
#include <react/renderer/element/Element.h>
#include <react/renderer/element/testUtils.h>
#include <react/renderer/graphics/Color.h>
#include <react/utils/ContextContainer.h>

#ifdef RN_SERIALIZABLE_STATE
#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/featureflags/ReactNativeFeatureFlagsDefaults.h>
#include <react/renderer/components/androidtextinput/AndroidTextInputProps.h>
#include <react/renderer/components/image/ImageProps.h>
#endif

namespace facebook::react {

class AnimatedPropsRegistryTest : public ::testing::Test {
 protected:
  ComponentBuilder builder =
      simpleComponentBuilder(std::make_shared<ContextContainer>());
  std::shared_ptr<ViewShadowNode> node =
      builder.build(Element<ViewShadowNode>().surfaceId(1).tag(10));
  AnimatedPropsRegistry registry;

  StampingSnapshot stamped;

  void SetUp() override {
    registry.initializeSurface(1);
  }

  uint64_t update(AnimatedProps props, bool hasLayoutUpdates = false) {
    std::unordered_map<SurfaceId, std::unordered_map<Tag, AnimationMutation>>
        surfaceUpdates;
    surfaceUpdates[1].emplace(
        10,
        AnimationMutation{
            .tag = 10,
            .family = node->getFamilyShared(),
            .props = std::move(props),
            .hasLayoutUpdates = hasLayoutUpdates});
    return registry.update(surfaceUpdates);
  }

  uint64_t updateRaw(folly::dynamic props) {
    return update(
        {.props = {},
         .rawProps = std::make_unique<RawProps>(std::move(props))});
  }

  void expectRawProps(const folly::dynamic& expected) {
    EXPECT_EQ(*snapshot().rawProps, expected);
  }

  static void expectRawPropsOf(
      const PropsSnapshot& props,
      const folly::dynamic& expected) {
    ASSERT_NE(props.rawProps, nullptr);
    EXPECT_EQ(*props.rawProps, expected);
  }

  static AnimatedProps opacity(Float value) {
    AnimatedPropsBuilder builder;
    builder.setOpacity(value);
    return builder.get();
  }

  PropsSnapshot& snapshot() {
    stamped = registry.takeStampingSnapshot(1);
    return stamped.props.at(10);
  }

  folly::dynamic newerDirectWrites(uint64_t stampNumber) {
    auto values = registry.newerDirectWrites(1, {{10, stampNumber}});
    return values.empty() ? folly::dynamic(nullptr) : values.at(0);
  }

  void expectNewerDirectWrites(
      uint64_t stampNumber,
      const folly::dynamic& expected) {
    EXPECT_EQ(newerDirectWrites(stampNumber), expected);
  }
};

TEST_F(AnimatedPropsRegistryTest, rawUpdatesDoNotAllocateTypedProps) {
  for (int frame = 0; frame < 10; frame++) {
    updateRaw(folly::dynamic::object("opacity", frame / 10.0));
    auto& props = snapshot();
    EXPECT_EQ(props.props, nullptr);
    EXPECT_TRUE(props.propNames.empty());
    EXPECT_EQ((*props.rawProps)["opacity"], frame / 10.0);
  }
}

TEST_F(AnimatedPropsRegistryTest, updatePreservesInputForSynchronousMount) {
  folly::dynamic rawProps = folly::dynamic::object("opacity", 0.25)(
      "transform",
      folly::dynamic::array(folly::dynamic::object("translateX", 10)));
  std::unordered_map<SurfaceId, std::unordered_map<Tag, AnimationMutation>>
      surfaceUpdates;
  surfaceUpdates[1].emplace(
      10,
      AnimationMutation{
          .tag = 10,
          .family = node->getFamilyShared(),
          .props = AnimatedProps{
              .props = {}, .rawProps = std::make_unique<RawProps>(rawProps)}});
  registry.update(surfaceUpdates);
  registry.update(surfaceUpdates);
  EXPECT_EQ(surfaceUpdates[1].at(10).props.rawProps->toDynamic(), rawProps);
  EXPECT_EQ(*snapshot().rawProps, rawProps);
}

TEST_F(AnimatedPropsRegistryTest, mergesPendingUpdatesPerKey) {
  updateRaw(
      folly::dynamic::object("opacity", 0.25)(
          "shadowOffset", folly::dynamic::object("width", 2)("height", 3)));
  updateRaw(
      folly::dynamic::object(
          "shadowOffset", folly::dynamic::object("width", 7)));
  expectRawProps(
      folly::dynamic::object("opacity", 0.25)(
          "shadowOffset", folly::dynamic::object("width", 7)));
}

TEST_F(AnimatedPropsRegistryTest, mergesCommittedUpdatesPerKey) {
  updateRaw(
      folly::dynamic::object("opacity", 0.25)(
          "shadowOffset", folly::dynamic::object("width", 2)("height", 3)));
  snapshot();
  updateRaw(
      folly::dynamic::object(
          "shadowOffset", folly::dynamic::object("width", 7)));
  expectRawProps(
      folly::dynamic::object("opacity", 0.25)(
          "shadowOffset", folly::dynamic::object("width", 7)));
}

TEST_F(AnimatedPropsRegistryTest, nullResetsArePreservedInPendingProps) {
  updateRaw(folly::dynamic::object("opacity", 0.25)("borderRadius", 4));
  updateRaw(folly::dynamic::object("opacity", nullptr));
  expectRawProps(folly::dynamic::object("opacity", nullptr)("borderRadius", 4));
}

TEST_F(AnimatedPropsRegistryTest, nullResetsArePreservedInCommittedProps) {
  updateRaw(folly::dynamic::object("opacity", 0.25)("borderRadius", 4));
  snapshot();
  updateRaw(folly::dynamic::object("opacity", nullptr));
  expectRawProps(folly::dynamic::object("opacity", nullptr)("borderRadius", 4));
  updateRaw(folly::dynamic::object("opacity", 0.5));
  expectRawProps(folly::dynamic::object("opacity", 0.5)("borderRadius", 4));
}

TEST_F(AnimatedPropsRegistryTest, arraysReplaceInsteadOfMerging) {
  updateRaw(
      folly::dynamic::object(
          "transform",
          folly::dynamic::array(
              folly::dynamic::object("translateX", 2),
              folly::dynamic::object("scale", 3))));
  snapshot();
  auto transform =
      folly::dynamic::array(folly::dynamic::object("rotate", "1rad"));
  updateRaw(folly::dynamic::object("transform", transform));
  EXPECT_EQ((*snapshot().rawProps)["transform"], transform);
}

TEST_F(AnimatedPropsRegistryTest, valuesReplaceRegardlessOfType) {
  updateRaw(folly::dynamic::object("value", 5));
  snapshot();
  folly::dynamic object = folly::dynamic::object("width", 2)("height", nullptr);
  updateRaw(folly::dynamic::object("value", object));
  expectRawProps(folly::dynamic::object("value", object));
  updateRaw(folly::dynamic::object("value", 7));
  expectRawProps(folly::dynamic::object("value", 7));
}

TEST_F(AnimatedPropsRegistryTest, typedPropsCanFollowRawProps) {
  updateRaw(folly::dynamic::object("opacity", 0.25));
  snapshot();
  AnimatedPropsBuilder builder;
  builder.setOpacity(0.75);
  update(builder.get());
  auto& props = snapshot();
  ASSERT_NE(props.props, nullptr);
  EXPECT_FLOAT_EQ(props.props->opacity, 0.75);
  EXPECT_TRUE(props.propNames.contains(OPACITY));
  EXPECT_FALSE(props.rawProps->count("opacity"));
  BaseViewProps viewProps;
  updateProp(OPACITY, viewProps, props);
  EXPECT_FLOAT_EQ(viewProps.opacity, 0.75);
}

TEST_F(AnimatedPropsRegistryTest, rawPropsCanFollowTypedProps) {
  AnimatedPropsBuilder builder;
  builder.setOpacity(0.75);
  update(builder.get());
  snapshot();
  updateRaw(folly::dynamic::object("opacity", 0.25));
  auto& props = snapshot();
  EXPECT_FALSE(props.propNames.contains(OPACITY));
  EXPECT_EQ((*props.rawProps)["opacity"], 0.25);
}

TEST_F(AnimatedPropsRegistryTest, rawPropsFollowPendingTypedProps) {
  AnimatedPropsBuilder builder;
  builder.setOpacity(0.75);
  builder.setShadowRadius(2);
  update(builder.get());
  updateRaw(folly::dynamic::object("opacity", nullptr));

  auto& props = snapshot();

  EXPECT_EQ(props.propNames, std::unordered_set<PropName>{SHADOW_RADIUS});
  expectRawPropsOf(props, folly::dynamic::object("opacity", nullptr));
}

TEST_F(AnimatedPropsRegistryTest, unrelatedRawPropsKeepTypedProps) {
  for (bool snapshotEachWrite : {false, true}) {
    updateRaw(folly::dynamic::object("opacity", 0.25));
    if (snapshotEachWrite) {
      snapshot();
    }
    update(opacity(0.75));
    if (snapshotEachWrite) {
      snapshot();
    }
    updateRaw(folly::dynamic::object("backgroundColor", 0xff0000ff));

    auto& props = snapshot();

    EXPECT_TRUE(props.propNames.contains(OPACITY));
    EXPECT_FLOAT_EQ(props.props->opacity, 0.75);
    EXPECT_FALSE(props.rawProps->count("opacity"));
  }
}

TEST_F(AnimatedPropsRegistryTest, rawEdgesSupersedeOnlyTheirTypedEdges) {
  for (bool snapshotEachWrite : {false, true}) {
    auto writeMargin = [&](yoga::Edge edge, float points) {
      CascadedRectangleEdges<yoga::StyleLength> margins;
      (edge == yoga::Edge::Left ? margins.left : margins.right) =
          yoga::StyleLength::points(points);
      AnimatedPropsBuilder builder;
      builder.setMargin(margins);
      update(builder.get());
      if (snapshotEachWrite) {
        snapshot();
      }
    };
    writeMargin(yoga::Edge::Left, 10);
    updateRaw(folly::dynamic::object("marginLeft", 20));
    if (snapshotEachWrite) {
      snapshot();
    }
    writeMargin(yoga::Edge::Right, 30);

    auto& props = snapshot();

    EXPECT_EQ(
        props.layoutEdges.at(MARGIN),
        std::unordered_set<yoga::Edge>{yoga::Edge::Right});
    EXPECT_EQ((*props.rawProps)["marginLeft"], 20);
  }
}

TEST_F(AnimatedPropsRegistryTest, rawGapKeepsTypedRowGap) {
  AnimatedPropsBuilder builder;
  builder.setRowGap(yoga::StyleLength::points(4));
  update(builder.get());
  updateRaw(folly::dynamic::object("gap", 8));
  EXPECT_TRUE(snapshot().propNames.contains(ROW_GAP));

  updateRaw(folly::dynamic::object("rowGap", nullptr));

  EXPECT_FALSE(snapshot().propNames.contains(ROW_GAP));
}

TEST_F(AnimatedPropsRegistryTest, layoutOnlyWritesRecordNoDirectWrites) {
  AnimatedPropsBuilder builder;
  builder.setWidth(yoga::StyleSizeLength::points(100));

  update(builder.get(), true);

  EXPECT_FALSE(registry.hasDirectWrites(1));
}

TEST_F(AnimatedPropsRegistryTest, mergesTypedUpdatesAcrossCommits) {
  AnimatedPropsBuilder builder;
  builder.setOpacity(0.75);
  builder.setShadowRadius(2);
  update(builder.get());
  snapshot();
  builder.setOpacity(0.5);
  update(builder.get());
  auto& props = snapshot();
  ASSERT_NE(props.props, nullptr);
  EXPECT_FLOAT_EQ(props.props->opacity, 0.5);
  EXPECT_FLOAT_EQ(props.props->shadowRadius, 2);
  EXPECT_EQ(props.propNames.size(), 2u);
}

TEST_F(AnimatedPropsRegistryTest, jsiRawPropsAreConverted) {
  auto runtime = hermes::makeHermesRuntime();
  {
    auto object = jsi::Object(*runtime);
    object.setProperty(*runtime, "opacity", 0.25);
    update(
        {.props = {},
         .rawProps = std::make_unique<RawProps>(
             *runtime, jsi::Value(*runtime, object))});
    object.setProperty(*runtime, "opacity", 0.5);
    update(
        {.props = {},
         .rawProps = std::make_unique<RawProps>(
             *runtime, jsi::Value(*runtime, object))});
  }
  runtime.reset();
  expectRawProps(folly::dynamic::object("opacity", 0.5));
  EXPECT_EQ(snapshot().props, nullptr);
}

TEST_F(AnimatedPropsRegistryTest, stoppedSurfacesIgnoreUpdates) {
  registry.clearOnSurfaceStop(1);
  updateRaw(folly::dynamic::object("opacity", 0.5));
  registry.initializeSurface(1);
  EXPECT_TRUE(registry.takeStampingSnapshot(1).props.empty());
  EXPECT_EQ(newerDirectWrites(0), nullptr);
}

TEST_F(AnimatedPropsRegistryTest, aStampCoversEveryWriteBeforeIt) {
  updateRaw(folly::dynamic::object("opacity", 0.25));
  auto number = updateRaw(folly::dynamic::object("borderRadius", 4));

  snapshot();

  EXPECT_EQ(stamped.number, number);
  expectNewerDirectWrites(stamped.number, folly::dynamic::object());
}

TEST_F(AnimatedPropsRegistryTest, newerDirectWritesAreTheLatestValues) {
  auto first = updateRaw(folly::dynamic::object("opacity", 0.25));
  updateRaw(folly::dynamic::object("opacity", 0.5)("borderRadius", 4));
  updateRaw(folly::dynamic::object("borderRadius", 6));

  expectNewerDirectWrites(
      first, folly::dynamic::object("opacity", 0.5)("borderRadius", 6));
  EXPECT_EQ(
      registry.newerDirectWrites(1, {{11, 0}}),
      std::vector<folly::dynamic>{folly::dynamic::object()});
}

TEST_F(AnimatedPropsRegistryTest, typedWritesArePackedWhenQueried) {
  AnimatedPropsBuilder builder;
  builder.setOpacity(0.75);
  auto typed = update(builder.get());
  auto raw = updateRaw(folly::dynamic::object("opacity", 0.25));

  expectNewerDirectWrites(0, folly::dynamic::object("opacity", 0.25));
  expectNewerDirectWrites(raw, folly::dynamic::object());

  builder.setOpacity(0.5);
  update(builder.get());

  expectNewerDirectWrites(typed, folly::dynamic::object("opacity", 0.5));
}

TEST_F(AnimatedPropsRegistryTest, committedViewsRecordTheirPackedProps) {
  AnimatedPropsBuilder builder;
  builder.setWidth(yoga::StyleSizeLength::points(100));
  builder.setOpacity(0.5);

  update(builder.get(), true);

  expectNewerDirectWrites(0, folly::dynamic::object("opacity", 0.5));
}

TEST_F(AnimatedPropsRegistryTest, surfaceStopDropsDirectWrites) {
  updateRaw(folly::dynamic::object("opacity", 0.5));

  registry.clearOnSurfaceStop(1);

  EXPECT_EQ(newerDirectWrites(0), nullptr);
  EXPECT_FALSE(registry.shouldOverridePullTransaction());
}

TEST_F(AnimatedPropsRegistryTest, stampingSnapshotsAreCopies) {
  updateRaw(folly::dynamic::object("opacity", 0.25));
  auto first = registry.takeStampingSnapshot(1);

  updateRaw(folly::dynamic::object("opacity", 0.5));
  snapshot();

  EXPECT_EQ(
      *first.props.at(10).rawProps,
      folly::dynamic(folly::dynamic::object("opacity", 0.25)));
}

TEST_F(AnimatedPropsRegistryTest, pulledMutationsGetNewerDirectWrites) {
  auto stamp = updateRaw(folly::dynamic::object("opacity", 0.25));
  updateRaw(folly::dynamic::object("opacity", 0.5));
  auto pull = [&](uint64_t stampNumber) {
    TransactionTelemetry telemetry;
    telemetry.setAnimationStampNumber(stampNumber);
    auto transaction = registry.pullTransaction(
        1,
        1,
        telemetry,
        {ShadowViewMutation::UpdateMutation(
            ShadowView(*node), ShadowView(*node), 1)});
    return static_cast<const ViewProps&>(
               *transaction->getMutations().at(0).newChildShadowView.props)
        .opacity;
  };

  EXPECT_EQ(pull(stamp), 0.5f);
  EXPECT_EQ(pull(stamp + 1), 1.0f);
}

TEST_F(AnimatedPropsRegistryTest, expiredViewsAreDroppedOnTheNextSnapshot) {
  {
    auto view = builder.build(Element<ViewShadowNode>().surfaceId(1).tag(11));
    std::unordered_map<SurfaceId, std::unordered_map<Tag, AnimationMutation>>
        surfaceUpdates;
    surfaceUpdates[1].emplace(
        11,
        AnimationMutation{
            .tag = 11,
            .family = view->getFamilyShared(),
            .props = {
                .props = {},
                .rawProps = std::make_unique<RawProps>(
                    folly::dynamic::object("opacity", 0.5))}});
    registry.update(surfaceUpdates);
    registry.takeStampingSnapshot(1);
    registry.clear(1);
  }
  EXPECT_TRUE(registry.hasDirectWrites(1));

  registry.takeStampingSnapshot(1);

  EXPECT_FALSE(registry.hasDirectWrites(1));
}

#ifdef RN_SERIALIZABLE_STATE
TEST_F(AnimatedPropsRegistryTest, restoringDirectWritesSendsReactsValues) {
  updateRaw(folly::dynamic::object("opacity", 0.25)("testID", "written"));
  auto props = node->getComponentDescriptor().cloneProps(
      PropsParserContext{1, *node->getContextContainer()},
      node->getProps(),
      RawProps(folly::dynamic::object("testID", "written")));

  EXPECT_EQ(
      registry.restoreDirectWrites(1, 10, props, node->getProps()),
      folly::dynamic(
          folly::dynamic::object("opacity", 1.0)("testID", "written")));
  EXPECT_EQ(
      registry.restoreDirectWrites(1, 11, props, nullptr),
      folly::dynamic(folly::dynamic::object()));
}

TEST_F(AnimatedPropsRegistryTest, restoringIncludesChangesSincePreallocation) {
  updateRaw(folly::dynamic::object("opacity", 0.7));
  PropsParserContext context{1, *node->getContextContainer()};
  const auto& componentDescriptor = node->getComponentDescriptor();
  auto preallocatedProps = componentDescriptor.cloneProps(
      context,
      node->getProps(),
      RawProps(folly::dynamic::object("opacity", 0.2)));
  auto props = componentDescriptor.cloneProps(
      context,
      preallocatedProps,
      RawProps(folly::dynamic::object("opacity", 0.7)));

  EXPECT_EQ(
      registry.restoreDirectWrites(1, 10, props, props),
      folly::dynamic(folly::dynamic::object()));
  auto preallocated =
      registry.restoreDirectWrites(1, 10, props, preallocatedProps);
  auto created = registry.restoreDirectWrites(1, 10, props, nullptr);
  EXPECT_NEAR(preallocated["opacity"].asDouble(), 0.7, 0.0001);
  EXPECT_NEAR(created["opacity"].asDouble(), 0.7, 0.0001);
}

TEST_F(AnimatedPropsRegistryTest, restoringLegacyTransformsSendsDefaults) {
  updateRaw(folly::dynamic::object("scaleX", 2)("translateY", 10));

  EXPECT_EQ(
      registry.restoreDirectWrites(1, 10, node->getProps(), nullptr),
      folly::dynamic(folly::dynamic::object("scaleX", 1.0)("translateY", 0.0)));
}

namespace {

class TintColorFlags : public ReactNativeFeatureFlagsDefaults {
 public:
  explicit TintColorFlags(bool transparentTintColor)
      : transparentTintColor_(transparentTintColor) {}

  bool enableImageTransparentTintColor() override {
    return transparentTintColor_;
  }

 private:
  bool transparentTintColor_;
};

} // namespace

TEST(RestoredValuesSerializationTest, anUnsetImageTintIsNull) {
  for (bool transparentTintColor : {false, true}) {
    ReactNativeFeatureFlags::dangerouslyReset();
    ReactNativeFeatureFlags::override(
        std::make_unique<TintColorFlags>(transparentTintColor));
    ImageProps tinted;
    tinted.tintColor = colorFromRGBA(255, 0, 0, 255);
    ImageProps transparent;
    transparent.tintColor = colorFromRGBA(0, 0, 0, 0);
    ImageProps unset;

    EXPECT_EQ(unset.getDiffProps(&tinted)["tintColor"], nullptr);
    EXPECT_EQ(transparent.getDiffProps(&tinted)["tintColor"], 0);
  }
  ReactNativeFeatureFlags::dangerouslyReset();
}

TEST(RestoredValuesSerializationTest, anUnsetTextInputColorIsNull) {
  AndroidTextInputProps colored;
  colored.textAttributes.foregroundColor = colorFromRGBA(255, 0, 0, 255);
  AndroidTextInputProps unset;

  EXPECT_EQ(unset.getDiffProps(&colored)["color"], nullptr);
  EXPECT_EQ(
      colored.getDiffProps(&unset)["color"], *colorFromRGBA(255, 0, 0, 255));
}
#endif

} // namespace facebook::react
