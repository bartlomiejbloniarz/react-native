/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <react/renderer/components/view/ViewShadowNode.h>
#include <react/renderer/element/ComponentBuilder.h>
#include <react/renderer/element/Element.h>
#include <react/renderer/element/testUtils.h>
#include <react/renderer/mounting/MountingCoordinator.h>
#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/mounting/ShadowTreeDelegate.h>

namespace facebook::react {

namespace {

// Pulls synchronously mounted revisions right away, as the UI thread does.
class SynchronousMountingDelegate : public ShadowTreeDelegate {
 public:
  mutable std::function<void()> onNextDidCommit;
  mutable std::vector<std::optional<MountingTransaction>>
      synchronousTransactions;
  mutable int finishedTransactions{0};

  RootShadowNode::Unshared shadowTreeWillCommit(
      const ShadowTree& /*shadowTree*/,
      const RootShadowNode::Shared& /*oldRootShadowNode*/,
      const RootShadowNode::Unshared& newRootShadowNode,
      const ShadowTree::CommitOptions& /*commitOptions*/) const override {
    return newRootShadowNode;
  }

  void shadowTreeDidCommit(
      const ShadowTree& /*shadowTree*/,
      const RootShadowNode::Shared& /*rootShadowNode*/,
      const std::vector<const LayoutableShadowNode*>& /*affectedNodes*/)
      const override {
    if (auto callback = std::exchange(onNextDidCommit, nullptr)) {
      callback();
    }
  }

  void shadowTreeDidFinishTransaction(
      std::shared_ptr<const MountingCoordinator> mountingCoordinator,
      bool mountSynchronously) const override {
    finishedTransactions++;
    if (mountSynchronously) {
      synchronousTransactions.push_back(mountingCoordinator->pullTransaction());
    }
  }

  void shadowTreeDidFinishReactCommit(
      const ShadowTree& /*shadowTree*/) const override {}
};

} // namespace

class MountingCoordinatorPushTest : public ::testing::Test {
 protected:
  MountingCoordinatorPushTest()
      : builder_(simpleComponentBuilder(contextContainer_)),
        reactView_(buildView(10)),
        nativeView_(buildView(11)),
        shadowTree_(
            SurfaceId{1},
            LayoutConstraints{},
            LayoutContext{},
            delegate_,
            *contextContainer_) {
    shadowTree_.commit(
        [&](const RootShadowNode& root) {
          return std::static_pointer_cast<RootShadowNode>(
              root.ShadowNode::clone(
                  {.children = std::make_shared<
                       std::vector<std::shared_ptr<const ShadowNode>>>(
                       std::vector<std::shared_ptr<const ShadowNode>>{
                           reactView_, nativeView_})}));
        },
        {});
  }

  std::shared_ptr<ViewShadowNode> buildView(Tag tag) const {
    auto props = std::make_shared<ViewShadowNodeProps>();
    props->nativeId = std::to_string(tag);
    return builder_.build(
        Element<ViewShadowNode>().tag(tag).surfaceId(1).props(props));
  }

  void commitOpacity(
      const ShadowNode& view,
      Float opacity,
      bool mountSynchronously) {
    shadowTree_.commit(
        [&](const RootShadowNode& root) {
          return std::static_pointer_cast<RootShadowNode>(
              root.cloneTree(view.getFamily(), [&](const ShadowNode& node) {
                PropsParserContext parserContext{1, *contextContainer_};
                return node.clone(
                    {.props = node.getComponentDescriptor().cloneProps(
                         parserContext,
                         node.getProps(),
                         RawProps(
                             folly::dynamic::object("opacity", opacity)))});
              }));
        },
        {.mountSynchronously = mountSynchronously});
  }

  static std::optional<Size> rootSize(const MountingTransaction& transaction) {
    for (const auto& mutation : transaction.getMutations()) {
      if (mutation.type == ShadowViewMutation::Update &&
          mutation.newChildShadowView.tag == 1) {
        return mutation.newChildShadowView.layoutMetrics.frame.size;
      }
    }
    return std::nullopt;
  }

  std::shared_ptr<const MountingCoordinator> mountingCoordinator() const {
    return shadowTree_.getMountingCoordinator();
  }

  std::shared_ptr<ContextContainer> contextContainer_ =
      std::make_shared<ContextContainer>();
  ComponentBuilder builder_;
  std::shared_ptr<ViewShadowNode> reactView_;
  std::shared_ptr<ViewShadowNode> nativeView_;
  SynchronousMountingDelegate delegate_;
  ShadowTree shadowTree_;
};

// Revision N is accepted, and before it is pushed, revision N+1 is committed,
// pushed and mounted synchronously. Pushing N afterwards must not roll the
// mounted tree back to N.
TEST_F(MountingCoordinatorPushTest, stalePushAfterNewerMountIsIgnored) {
  delegate_.onNextDidCommit = [&]() {
    commitOpacity(*nativeView_, 0.5, /*mountSynchronously*/ true);
  };
  commitOpacity(*reactView_, 0.5, /*mountSynchronously*/ false);

  ASSERT_EQ(delegate_.synchronousTransactions.size(), 2);
  ASSERT_TRUE(delegate_.synchronousTransactions.back().has_value());
  EXPECT_EQ(delegate_.synchronousTransactions.back()->getMutations().size(), 2);
  EXPECT_EQ(delegate_.finishedTransactions, 2);
  EXPECT_FALSE(mountingCoordinator()->pullTransaction().has_value());
}

// Same race with the newer revision coming from a surface size change, as
// SurfaceHandler::constraintLayout commits it: the stale push must not lay the
// root out for the old size again.
TEST_F(MountingCoordinatorPushTest, stalePushAfterLayoutConstraintsChange) {
  const Size newSize{.width = 200, .height = 100};
  delegate_.onNextDidCommit = [&]() {
    shadowTree_.commit(
        [&](const RootShadowNode& root) {
          PropsParserContext parserContext{1, *contextContainer_};
          return root.clone(
              parserContext,
              LayoutConstraints{.minimumSize = newSize, .maximumSize = newSize},
              LayoutContext{});
        },
        {});
  };
  commitOpacity(*reactView_, 0.5, /*mountSynchronously*/ false);

  ASSERT_EQ(delegate_.synchronousTransactions.size(), 2);
  ASSERT_TRUE(delegate_.synchronousTransactions.back().has_value());
  auto mountedRootSize = rootSize(*delegate_.synchronousTransactions.back());
  ASSERT_EQ(mountedRootSize, newSize);
  if (auto transaction = mountingCoordinator()->pullTransaction()) {
    mountedRootSize = rootSize(*transaction).value_or(*mountedRootSize);
  }
  EXPECT_EQ(mountedRootSize, newSize);
}

TEST_F(MountingCoordinatorPushTest, resumeWithoutNewCommitsMountsNothing) {
  shadowTree_.setCommitMode(ShadowTree::CommitMode::Suspended);
  shadowTree_.setCommitMode(ShadowTree::CommitMode::Normal);

  EXPECT_EQ(delegate_.finishedTransactions, 1);
  EXPECT_FALSE(mountingCoordinator()->pullTransaction().has_value());
}

TEST_F(MountingCoordinatorPushTest, resumeMountsCommitsMadeWhileSuspended) {
  shadowTree_.setCommitMode(ShadowTree::CommitMode::Suspended);
  commitOpacity(*reactView_, 0.5, /*mountSynchronously*/ true);
  shadowTree_.setCommitMode(ShadowTree::CommitMode::Normal);

  ASSERT_EQ(delegate_.synchronousTransactions.size(), 2);
  ASSERT_TRUE(delegate_.synchronousTransactions.back().has_value());
  EXPECT_EQ(delegate_.synchronousTransactions.back()->getMutations().size(), 1);
}

} // namespace facebook::react
