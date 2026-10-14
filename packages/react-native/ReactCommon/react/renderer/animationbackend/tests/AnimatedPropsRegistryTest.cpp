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
#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/element/ComponentBuilder.h>
#include <react/renderer/element/Element.h>
#include <react/renderer/element/testUtils.h>
#include <react/renderer/graphics/Color.h>
#include <react/utils/ContextContainer.h>
#include <atomic>
#include <thread>

namespace facebook::react {

class AnimatedPropsRegistryTest : public ::testing::Test {
 protected:
  ComponentBuilder builder =
      simpleComponentBuilder(std::make_shared<ContextContainer>());
  std::shared_ptr<ViewShadowNode> node =
      builder.build(Element<ViewShadowNode>().surfaceId(1).tag(10));
  AnimatedPropsRegistry registry;

  void SetUp() override {
    registry.initializeSurface(1);
  }

  void update(AnimatedProps props) {
    std::vector<AnimationMutations> batches(1);
    batches[0].batch.push_back(
        AnimationMutation{
            .tag = 10,
            .family = node->getFamilyShared(),
            .props = std::move(props)});
    registry.update(batches);
  }

  void updateRaw(folly::dynamic props) {
    update(
        {.props = {},
         .rawProps = std::make_unique<RawProps>(std::move(props))});
  }

  void expectRawProps(const folly::dynamic& expected) {
    EXPECT_EQ(*snapshot().rawProps, expected);
  }

  // The view's stamped props, as the commit hook reads them.
  const PropsSnapshot& snapshot() {
    readSnapshot = registry.getMap(1).second.at(10);
    return *readSnapshot;
  }

  std::shared_ptr<const PropsSnapshot> readSnapshot;
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
  std::vector<AnimationMutations> batches(1);
  batches[0].batch.push_back(
      AnimationMutation{
          .tag = 10,
          .family = node->getFamilyShared(),
          .props = AnimatedProps{
              .props = {}, .rawProps = std::make_unique<RawProps>(rawProps)}});
  registry.update(batches);
  registry.update(batches);
  EXPECT_EQ(batches[0].batch[0].props.rawProps->toDynamic(), rawProps);
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
  EXPECT_TRUE(!props.rawProps || props.rawProps->count("opacity") == 0);
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
  EXPECT_TRUE(registry.getMap(1).second.empty());
}

constexpr AnimationFrontendId kAnimated = 1;
constexpr AnimationFrontendId kOtherAnimated = 2;
constexpr AnimationFrontendId kReanimated = 3;

class AnimatedPropsRegistryFrontendTest : public AnimatedPropsRegistryTest {
 protected:
  void write(AnimationFrontendId frontendId, AnimatedProps props) {
    write(frontendId, std::move(props), node);
  }

  void write(
      AnimationFrontendId frontendId,
      AnimatedProps props,
      const std::shared_ptr<ViewShadowNode>& view) {
    FrontendMutations batch{
        .frontendId = frontendId,
        .commitDirectProps = frontendId == kReanimated};
    batch.mutations.batch.push_back(
        AnimationMutation{
            .tag = view->getTag(),
            .family = view->getFamilyShared(),
            .props = std::move(props)});
    registry.update(batch);
  }

  void write(
      AnimationFrontendId frontendId,
      const folly::dynamic& rawProps,
      const std::shared_ptr<ViewShadowNode>& view = nullptr) {
    write(
        frontendId,
        {.props = {}, .rawProps = std::make_unique<RawProps>(rawProps)},
        view ? view : node);
  }

  std::set<SurfaceId> release(
      AnimationFrontendId frontendId,
      std::vector<std::string> keys,
      bool commitValues = false) {
    std::vector<AnimationMutation> restoredWrites;
    return registry.release(
        frontendId,
        {{.family = node->getFamilyShared(),
          .keys = std::move(keys),
          .commitValues = commitValues}},
        restoredWrites);
  }

  size_t stampedViews() {
    return registry.surfaceContexts_.at(1).stamped.size();
  }

  void stopStamping() {
    registry.stopStamping(1, registry.releaseCommit(1).release);
  }

  PulledValues pull(uint64_t release, std::vector<Tag> tags = {10}) {
    return registry.pull(1, release, tags);
  }

  static folly::dynamic opacity(double value) {
    return folly::dynamic::object("opacity", value);
  }
};

TEST_F(AnimatedPropsRegistryFrontendTest, uncommittedValuesAreNotStamped) {
  write(kAnimated, opacity(0.5));

  EXPECT_TRUE(registry.getMap(1).second.empty());
  EXPECT_EQ(pull(0).overlaid.at(10), opacity(0.5));
}

TEST_F(AnimatedPropsRegistryFrontendTest, committedValuesAreStamped) {
  write(kReanimated, opacity(0.5));

  expectRawProps(opacity(0.5));
  EXPECT_TRUE(pull(0).overlaid.empty());
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    anUncommittedWriteRemovesTheStampedValueOfItsKey) {
  write(
      kReanimated,
      folly::dynamic::object("opacity", 0.3)("backgroundColor", 0xff0000ff));
  registry.getMap(1);
  write(kAnimated, opacity(0.5));

  expectRawProps(
      folly::dynamic(folly::dynamic::object("backgroundColor", 0xff0000ff)));
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aCommittedWriteSupersedesAnUncommittedValue) {
  write(kAnimated, opacity(0.5));
  write(kReanimated, opacity(0.3));

  expectRawProps(opacity(0.3));
  EXPECT_TRUE(pull(0).overlaid.empty());
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    anUncommittedWriteRemovesAStampedTypedValueOfItsKey) {
  AnimatedPropsBuilder propsBuilder;
  propsBuilder.setOpacity(0.3);
  write(kReanimated, propsBuilder.get());
  write(kReanimated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  registry.getMap(1);
  write(kAnimated, opacity(0.5));

  EXPECT_FALSE(snapshot().propNames.contains(OPACITY));
  expectRawProps(folly::dynamic::object("backgroundColor", 0xff0000ff));
}

TEST_F(AnimatedPropsRegistryFrontendTest, aReadSnapshotOutlivesLaterChanges) {
  write(kReanimated, opacity(0.3));
  auto firstReader = registry.getMap(1).second.at(10);
  write(kAnimated, opacity(0.5));
  write(kReanimated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  auto secondReader = registry.getMap(1).second.at(10);
  registry.clear(1);

  EXPECT_EQ(*firstReader->rawProps, opacity(0.3));
  EXPECT_EQ(
      *secondReader->rawProps,
      folly::dynamic(folly::dynamic::object("backgroundColor", 0xff0000ff)));
}

TEST_F(AnimatedPropsRegistryFrontendTest, concurrentReadersOwnTheirSnapshots) {
  write(kReanimated, opacity(0.3));
  std::atomic<bool> isWriting{true};
  std::thread reader([&]() {
    while (isWriting) {
      auto [families, snapshots] = registry.getMap(1);
      if (auto it = snapshots.find(10);
          it != snapshots.end() && it->second->rawProps) {
        folly::dynamic rawProps = *it->second->rawProps;
        EXPECT_TRUE(rawProps.isObject());
      }
    }
  });
  for (int frame = 0; frame < 2000; frame++) {
    write(kReanimated, opacity(frame / 2000.0));
    write(kAnimated, opacity(frame / 2000.0));
    registry.getMap(1);
  }
  isWriting = false;
  reader.join();
}

TEST_F(AnimatedPropsRegistryFrontendTest, aReleaseEndsOnlyItsWritersKeys) {
  write(kAnimated, opacity(0.5));
  write(kOtherAnimated, folly::dynamic::object("backgroundColor", 0xff0000ff));

  EXPECT_EQ(release(kAnimated, {"opacity", "backgroundColor"}).size(), 1u);
  auto pulled = pull(1);

  EXPECT_EQ(pulled.restored.at(10), opacity(0.5));
  EXPECT_EQ(
      pulled.overlaid.at(10),
      folly::dynamic(folly::dynamic::object("backgroundColor", 0xff0000ff)));
}

TEST_F(AnimatedPropsRegistryFrontendTest, aReleaseOfUnwrittenKeysIsANoOp) {
  write(kAnimated, opacity(0.5));

  EXPECT_TRUE(release(kOtherAnimated, {"opacity"}).empty());
  EXPECT_TRUE(release(kAnimated, {"transform"}).empty());
  EXPECT_TRUE(registry.releaseCommit(1).families.empty());
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aReleasedValueWinsUntilAPullCoversTheRelease) {
  write(kAnimated, opacity(0.5));
  release(kAnimated, {"opacity"});
  auto releaseCommit = registry.releaseCommit(1);

  EXPECT_EQ(releaseCommit.release, 1u);
  ASSERT_EQ(releaseCommit.families.size(), 1u);
  EXPECT_TRUE(releaseCommit.adoptedValues.empty());
  EXPECT_EQ(pull(0).overlaid.at(10), opacity(0.5));
  EXPECT_EQ(pull(1).restored.at(10), opacity(0.5));
  auto later = pull(1);
  EXPECT_TRUE(later.overlaid.empty());
  EXPECT_TRUE(later.restored.empty());
}

TEST_F(AnimatedPropsRegistryFrontendTest, aRestorationWaitsForItsViewsPull) {
  write(kAnimated, opacity(0.5));
  release(kAnimated, {"opacity"});

  EXPECT_TRUE(pull(1, {}).restored.empty());
  EXPECT_EQ(pull(1).restored.at(10), opacity(0.5));
}

TEST_F(AnimatedPropsRegistryFrontendTest, aNewerWriteSupersedesARelease) {
  write(kAnimated, opacity(0.5));
  release(kAnimated, {"opacity"});
  write(kOtherAnimated, opacity(0.7));

  EXPECT_TRUE(registry.releaseCommit(1).families.empty());
  auto pulled = pull(1);
  EXPECT_EQ(pulled.overlaid.at(10), opacity(0.7));
  EXPECT_TRUE(pulled.restored.empty());
}

TEST_F(AnimatedPropsRegistryFrontendTest, anAdoptedValueIsCommitted) {
  write(kAnimated, opacity(0.5));

  EXPECT_EQ(release(kAnimated, {"opacity"}, true).size(), 1u);
  EXPECT_EQ(registry.releaseCommit(1).adoptedValues.at(10), opacity(0.5));
  EXPECT_EQ(pull(0).overlaid.at(10), opacity(0.5));
  auto pulled = pull(1);
  EXPECT_TRUE(pulled.overlaid.empty());
  EXPECT_TRUE(pulled.restored.empty());
  EXPECT_TRUE(registry.releaseCommit(1).families.empty());
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    anAdoptionWithoutValuesNeedsNoCommit) {
  EXPECT_TRUE(release(kAnimated, {"opacity"}, true).empty());
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aLaterReleaseReplacesAPendingAdoption) {
  write(kAnimated, opacity(0.5));
  release(kAnimated, {"opacity"}, true);
  release(kAnimated, {"opacity"});

  auto releaseCommit = registry.releaseCommit(1);
  EXPECT_EQ(releaseCommit.release, 2u);
  EXPECT_TRUE(releaseCommit.adoptedValues.empty());
  EXPECT_EQ(pull(1).overlaid.at(10), opacity(0.5));
  EXPECT_EQ(pull(2).restored.at(10), opacity(0.5));
}

TEST_F(AnimatedPropsRegistryFrontendTest, surfaceStopDropsUncommittedValues) {
  write(kAnimated, opacity(0.5));
  release(kAnimated, {"opacity"});
  registry.clearOnSurfaceStop(1);
  registry.initializeSurface(1);

  EXPECT_TRUE(registry.releaseCommit(1).families.empty());
  EXPECT_TRUE(pull(1).restored.empty());
}

TEST_F(AnimatedPropsRegistryFrontendTest, endSyncKeepsUncommittedValues) {
  write(kAnimated, opacity(0.5));
  registry.clear(1);

  EXPECT_EQ(pull(0).overlaid.at(10), opacity(0.5));
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aLaterRawValueSupersedesAStampedTypedValue) {
  for (bool isConsumedBetween : {false, true}) {
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setOpacity(0.3);
    write(kReanimated, propsBuilder.get());
    if (isConsumedBetween) {
      registry.getMap(1);
    }
    write(kReanimated, opacity(0.5));

    EXPECT_FALSE(snapshot().propNames.contains(OPACITY));
    expectRawProps(opacity(0.5));
    registry.clearOnSurfaceStop(1);
    registry.initializeSurface(1);
  }
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aRawBorderKeyKeepsTheOtherTypedValuesOfItsGroup) {
  for (bool isConsumedBetween : {false, true}) {
    CascadedBorderRadii borderRadii;
    borderRadii.topLeft = ValueUnit(3, UnitType::Point);
    borderRadii.topRight = ValueUnit(50, UnitType::Percent);
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setBorderRadii(borderRadii);
    write(kReanimated, propsBuilder.get());
    if (isConsumedBetween) {
      registry.getMap(1);
    }
    write(kAnimated, folly::dynamic::object("borderTopLeftRadius", 5));

    EXPECT_FALSE(snapshot().propNames.contains(BORDER_RADII));
    expectRawProps(folly::dynamic::object("borderTopRightRadius", "50%"));
    registry.clearOnSurfaceStop(1);
    registry.initializeSurface(1);
  }
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aCommittedReleaseIsNotCommittedAgain) {
  write(kAnimated, opacity(0.5));
  release(kAnimated, {"opacity"}, true);
  registry.didCommitReleases(1, registry.releaseCommit(1).release);

  EXPECT_TRUE(registry.releaseCommit(1).families.empty());
  EXPECT_EQ(pull(0).overlaid.at(10), opacity(0.5));

  release(kAnimated, {"opacity"});
  EXPECT_EQ(registry.releaseCommit(1).families.size(), 1u);
}

TEST_F(AnimatedPropsRegistryFrontendTest, aCommittedReleaseKeepsALaterRelease) {
  write(kAnimated, opacity(0.5));
  release(kAnimated, {"opacity"}, true);
  auto releaseCommit = registry.releaseCommit(1);
  release(kAnimated, {"opacity"});
  registry.didCommitReleases(1, releaseCommit.release);

  EXPECT_EQ(registry.releaseCommit(1).families.size(), 1u);
}

TEST_F(AnimatedPropsRegistryFrontendTest, expiredViewsAreDropped) {
  auto view = builder.build(Element<ViewShadowNode>().surfaceId(1).tag(11));
  write(kAnimated, opacity(0.5), view);
  view.reset();

  EXPECT_TRUE(registry.pull(1, 0, {11}).overlaid.empty());
}

template <typename Group, typename Value>
void expectEveryMemberPacked(
    PropName propName,
    Group BaseViewProps::* group,
    const Value& value,
    const std::vector<std::pair<std::optional<Value> Group::*, std::string>>&
        members) {
  for (const auto& [member, key] : members) {
    BaseViewProps viewProps;
    (viewProps.*group).*member = value;
    auto packed = animationbackend::packBorderProp(propName, viewProps);
    EXPECT_EQ(packed.size(), 1u) << key;
    EXPECT_EQ(packed.count(key), 1u) << key;
  }
}

TEST(AnimatedPropsSerializerTest, everyBorderGroupMemberIsPacked) {
  using Colors = CascadedBorderColors;
  expectEveryMemberPacked<Colors, SharedColor>(
      BORDER_COLOR,
      &BaseViewProps::borderColors,
      colorFromRGBA(255, 0, 0, 255),
      {{&Colors::left, "borderLeftColor"},
       {&Colors::top, "borderTopColor"},
       {&Colors::right, "borderRightColor"},
       {&Colors::bottom, "borderBottomColor"},
       {&Colors::start, "borderStartColor"},
       {&Colors::end, "borderEndColor"},
       {&Colors::horizontal, "borderHorizontalColor"},
       {&Colors::vertical, "borderVerticalColor"},
       {&Colors::block, "borderBlockColor"},
       {&Colors::blockStart, "borderBlockStartColor"},
       {&Colors::blockEnd, "borderBlockEndColor"},
       {&Colors::all, "borderColor"}});
  using Styles = CascadedBorderStyles;
  expectEveryMemberPacked<Styles, BorderStyle>(
      BORDER_STYLES,
      &BaseViewProps::borderStyles,
      BorderStyle::Dashed,
      {{&Styles::left, "borderLeftStyle"},
       {&Styles::top, "borderTopStyle"},
       {&Styles::right, "borderRightStyle"},
       {&Styles::bottom, "borderBottomStyle"},
       {&Styles::start, "borderStartStyle"},
       {&Styles::end, "borderEndStyle"},
       {&Styles::horizontal, "borderHorizontalStyle"},
       {&Styles::vertical, "borderVerticalStyle"},
       {&Styles::block, "borderBlockStyle"},
       {&Styles::blockStart, "borderBlockStartStyle"},
       {&Styles::blockEnd, "borderBlockEndStyle"},
       {&Styles::all, "borderStyle"}});
  using Curves = CascadedBorderCurves;
  expectEveryMemberPacked<Curves, BorderCurve>(
      BORDER_CURVES,
      &BaseViewProps::borderCurves,
      BorderCurve::Continuous,
      {{&Curves::topLeft, "borderTopLeftCurve"},
       {&Curves::topRight, "borderTopRightCurve"},
       {&Curves::bottomLeft, "borderBottomLeftCurve"},
       {&Curves::bottomRight, "borderBottomRightCurve"},
       {&Curves::topStart, "borderTopStartCurve"},
       {&Curves::topEnd, "borderTopEndCurve"},
       {&Curves::bottomStart, "borderBottomStartCurve"},
       {&Curves::bottomEnd, "borderBottomEndCurve"},
       {&Curves::startStart, "borderStartStartCurve"},
       {&Curves::startEnd, "borderStartEndCurve"},
       {&Curves::endStart, "borderEndStartCurve"},
       {&Curves::endEnd, "borderEndEndCurve"},
       {&Curves::all, "borderCurve"}});
  using Radii = CascadedBorderRadii;
  expectEveryMemberPacked<Radii, ValueUnit>(
      BORDER_RADII,
      &BaseViewProps::borderRadii,
      ValueUnit(4, UnitType::Point),
      {{&Radii::topLeft, "borderTopLeftRadius"},
       {&Radii::topRight, "borderTopRightRadius"},
       {&Radii::bottomLeft, "borderBottomLeftRadius"},
       {&Radii::bottomRight, "borderBottomRightRadius"},
       {&Radii::topStart, "borderTopStartRadius"},
       {&Radii::topEnd, "borderTopEndRadius"},
       {&Radii::bottomStart, "borderBottomStartRadius"},
       {&Radii::bottomEnd, "borderBottomEndRadius"},
       {&Radii::startStart, "borderStartStartRadius"},
       {&Radii::startEnd, "borderStartEndRadius"},
       {&Radii::endStart, "borderEndStartRadius"},
       {&Radii::endEnd, "borderEndEndRadius"},
       {&Radii::all, "borderRadius"}});
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aTypedBorderGroupSupersedesEarlierRawValuesOfItsGroup) {
  for (bool isConsumedBetween : {false, true}) {
    write(kReanimated, folly::dynamic::object("borderTopLeftRadius", 5));
    if (isConsumedBetween) {
      registry.getMap(1);
    }
    CascadedBorderRadii borderRadii;
    borderRadii.topRight = ValueUnit(9, UnitType::Point);
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setBorderRadii(borderRadii);
    write(kReanimated, propsBuilder.get());
    write(kReanimated, folly::dynamic::object("borderTopRightRadius", 11));

    expectRawProps(folly::dynamic::object("borderTopRightRadius", 11));
    EXPECT_FALSE(snapshot().propNames.contains(BORDER_RADII));
    registry.clearOnSurfaceStop(1);
    registry.initializeSurface(1);
  }
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    anUnrelatedUncommittedWriteKeepsTheStampedSnapshot) {
  write(kReanimated, folly::dynamic::object("backgroundColor", 0xff0000ff));
  auto before = registry.getMap(1).second.at(10);
  write(kAnimated, opacity(0.5));

  EXPECT_EQ(registry.getMap(1).second.at(10), before);
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aTakenOverStampedViewIsNoLongerStamped) {
  write(kReanimated, opacity(0.3));
  registry.getMap(1);
  write(kAnimated, opacity(0.5));

  auto [families, snapshots] = registry.getMap(1);
  EXPECT_TRUE(families.empty());
  EXPECT_TRUE(snapshots.empty());
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aTypedGroupReplacesAConsumedTypedGroup) {
  auto writeRadius = [&](std::optional<ValueUnit> CascadedBorderRadii::* corner,
                         Float value) {
    CascadedBorderRadii borderRadii;
    borderRadii.*corner = ValueUnit(value, UnitType::Point);
    AnimatedPropsBuilder propsBuilder;
    propsBuilder.setBorderRadii(borderRadii);
    write(kReanimated, propsBuilder.get());
  };
  for (bool isConsumedBetween : {false, true}) {
    for (auto finalWriter : {kReanimated, kAnimated}) {
      writeRadius(&CascadedBorderRadii::topLeft, 5);
      registry.getMap(1);
      writeRadius(&CascadedBorderRadii::topRight, 9);
      if (isConsumedBetween) {
        registry.getMap(1);
      }
      write(finalWriter, folly::dynamic::object("borderTopRightRadius", 11));

      auto snapshots = registry.getMap(1).second;
      if (finalWriter == kReanimated) {
        EXPECT_EQ(
            *snapshots.at(10)->rawProps,
            folly::dynamic(folly::dynamic::object("borderTopRightRadius", 11)));
        EXPECT_FALSE(snapshots.at(10)->propNames.contains(BORDER_RADII));
      } else {
        EXPECT_TRUE(snapshots.empty());
      }
      registry.clearOnSurfaceStop(1);
      registry.initializeSurface(1);
    }
  }
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    aSupersededAndTakenOverViewIsNoLongerStamped) {
  write(kReanimated, opacity(0.3));
  registry.getMap(1);
  AnimatedPropsBuilder propsBuilder;
  propsBuilder.setOpacity(0.4);
  write(kReanimated, propsBuilder.get());
  write(kAnimated, opacity(0.5));

  auto [families, snapshots] = registry.getMap(1);
  EXPECT_TRUE(families.empty());
  EXPECT_TRUE(snapshots.empty());
}

TEST_F(AnimatedPropsRegistryFrontendTest, anAdoptedViewLeavesNoRecords) {
  auto view = builder.build(Element<ViewShadowNode>().surfaceId(1).tag(11));
  std::weak_ptr<const ShadowNodeFamily> family = view->getFamilyShared();
  write(kReanimated, opacity(0.5), view);
  registry.getMap(1);
  std::vector<AnimationMutation> restoredWrites;
  registry.release(
      kReanimated,
      {{.family = view->getFamilyShared(),
        .keys = {"opacity"},
        .commitValues = true}},
      restoredWrites);
  stopStamping();
  view.reset();

  auto [families, snapshots] = registry.getMap(1);
  EXPECT_TRUE(families.empty());
  EXPECT_TRUE(snapshots.empty());
  EXPECT_EQ(stampedViews(), 0u);
  EXPECT_TRUE(family.expired());
}

TEST_F(
    AnimatedPropsRegistryFrontendTest,
    repeatedAdoptionsAndTakeoversLeaveNoRecords) {
  for (int round = 0; round < 3; round++) {
    write(kReanimated, opacity(0.5));
    registry.getMap(1);
    write(kAnimated, opacity(0.6));
    EXPECT_EQ(stampedViews(), 0u);

    write(kReanimated, opacity(0.7));
    release(kReanimated, {"opacity"}, true);
    stopStamping();
    EXPECT_EQ(stampedViews(), 0u);
  }
}

TEST_F(AnimatedPropsRegistryFrontendTest, anEmptyWriteLeavesNoRecords) {
  for (Tag tag = 11; tag < 14; tag++) {
    auto view = builder.build(Element<ViewShadowNode>().surfaceId(1).tag(tag));
    std::weak_ptr<const ShadowNodeFamily> family = view->getFamilyShared();
    write(kReanimated, folly::dynamic::object(), view);
    write(kReanimated, AnimatedProps{}, view);
    registry.getMap(1);
    registry.clear(1);
    view.reset();

    auto [families, snapshots] = registry.getMap(1);
    EXPECT_TRUE(families.empty());
    EXPECT_TRUE(snapshots.empty());
    EXPECT_EQ(stampedViews(), 0u);
    EXPECT_TRUE(family.expired());
  }
}

TEST_F(AnimatedPropsRegistryFrontendTest, aTypedGroupWithoutValuesIsStamped) {
  CascadedBorderRadii borderRadii;
  AnimatedPropsBuilder propsBuilder;
  propsBuilder.setBorderRadii(borderRadii);
  write(kReanimated, propsBuilder.get());

  EXPECT_TRUE(
      registry.getMap(1).second.at(10)->propNames.contains(BORDER_RADII));
  EXPECT_EQ(stampedViews(), 1u);
}

TEST_F(AnimatedPropsRegistryFrontendTest, aReactCommitEndsReleasedStamping) {
  write(kReanimated, folly::dynamic::object("opacity", 0.5)("width", 10));
  registry.getMap(1);
  release(kReanimated, {"opacity"}, true);
  registry.clear(1);

  expectRawProps(folly::dynamic::object("width", 10));
}

TEST(AnimatedPropsSerializerTest, everyKeyOfATypedPropAddressesIt) {
  std::vector<std::pair<std::string, PropName>> keys{
      {"opacity", OPACITY},
      {"transform", TRANSFORM},
      {"transformOrigin", TRANSFORM_ORIGIN},
      {"backgroundColor", BACKGROUND_COLOR},
      {"shadowColor", SHADOW_COLOR},
      {"shadowOffset", SHADOW_OFFSET},
      {"shadowOpacity", SHADOW_OPACITY},
      {"shadowRadius", SHADOW_RADIUS},
      {"filter", FILTER},
      {"outlineColor", OUTLINE_COLOR},
      {"outlineOffset", OUTLINE_OFFSET},
      {"outlineStyle", OUTLINE_STYLE},
      {"outlineWidth", OUTLINE_WIDTH},
      {"pointerEvents", POINTER_EVENTS},
      {"isolation", ISOLATION},
      {"cursor", CURSOR},
      {"boxShadow", BOX_SHADOW},
      {"mixBlendMode", MIX_BLEND_MODE},
      {"backfaceVisibility", BACKFACE_VISIBILITY},
      {"width", WIDTH},
      {"height", HEIGHT},
      {"flex", FLEX},
      {"flexBasis", FLEX_BASIS},
      {"flexDirection", FLEX_DIRECTION},
      {"flexGrow", FLEX_GROW},
      {"flexShrink", FLEX_SHRINK},
      {"flexWrap", FLEX_WRAP},
      {"alignContent", ALIGN_CONTENT},
      {"alignItems", ALIGN_ITEMS},
      {"alignSelf", ALIGN_SELF},
      {"aspectRatio", ASPECT_RATIO},
      {"boxSizing", BOX_SIZING},
      {"display", DISPLAY},
      {"justifyContent", JUSTIFY_CONTENT},
      {"maxHeight", MAX_HEIGHT},
      {"maxWidth", MAX_WIDTH},
      {"minHeight", MIN_HEIGHT},
      {"minWidth", MIN_WIDTH},
      {"overflow", STYLE_OVERFLOW},
      {"position", POSITION_TYPE},
      {"zIndex", Z_INDEX},
      {"direction", DIRECTION},
      {"rowGap", ROW_GAP},
      {"columnGap", COLUMN_GAP},
  };
  // The edges updateProp copies for margin, padding, position and border width.
  for (const auto& edge :
       {"Left",
        "Top",
        "Right",
        "Bottom",
        "Start",
        "End",
        "Horizontal",
        "Vertical"}) {
    keys.emplace_back(std::string("margin") + edge, MARGIN);
    keys.emplace_back(std::string("padding") + edge, PADDING);
    keys.emplace_back(std::string("border") + edge + "Width", BORDER_WIDTH);
  }
  for (const auto& key :
       {"left",
        "top",
        "right",
        "bottom",
        "start",
        "end",
        "insetInline",
        "insetBlock"}) {
    keys.emplace_back(key, POSITION);
  }
  // The keys the border groups are written as.
  BaseViewProps viewProps;
  viewProps.borderRadii.topLeft = ValueUnit(1, UnitType::Point);
  viewProps.borderColors.blockStart = colorFromRGBA(1, 2, 3, 255);
  viewProps.borderCurves.endEnd = BorderCurve::Continuous;
  viewProps.borderStyles.horizontal = BorderStyle::Dashed;
  for (auto propName :
       {BORDER_RADII, BORDER_COLOR, BORDER_CURVES, BORDER_STYLES}) {
    auto packed = animationbackend::packBorderProp(propName, viewProps);
    EXPECT_FALSE(packed.empty());
    for (const auto& [key, _] : packed.items()) {
      keys.emplace_back(key.getString(), propName);
    }
  }
  for (const auto& [key, propName] : keys) {
    EXPECT_EQ(animationbackend::propNameOfKey(key), propName) << key;
  }
}

TEST_F(AnimatedPropsRegistryFrontendTest, aRawKeyReleasesATypedPosition) {
  CascadedRectangleEdges<yoga::StyleLength> position;
  position.horizontal = yoga::StyleLength::points(4);
  AnimatedPropsBuilder propsBuilder;
  propsBuilder.setPosition(position);
  write(kReanimated, propsBuilder.get());

  EXPECT_EQ(release(kReanimated, {"insetInline"}, true).size(), 1u);
}

} // namespace facebook::react
