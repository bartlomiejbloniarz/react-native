/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <functional>
#include <memory>

#include <gtest/gtest.h>

#include <react/renderer/mounting/MountingCoordinator.h>
#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/mounting/ShadowTreeDelegate.h>
#include <react/renderer/telemetry/TransactionTelemetry.h>

using namespace facebook::react;

namespace {

class WillCommitShadowTreeDelegate : public ShadowTreeDelegate {
 public:
  mutable std::function<void()> willCommit;

  RootShadowNode::Unshared shadowTreeWillCommit(
      const ShadowTree& /*shadowTree*/,
      const RootShadowNode::Shared& /*oldRootShadowNode*/,
      const RootShadowNode::Unshared& newRootShadowNode,
      const ShadowTree::CommitOptions& /*commitOptions*/) const override {
    if (auto callback = std::exchange(willCommit, nullptr)) {
      callback();
    }
    return newRootShadowNode;
  }

  void shadowTreeDidFinishTransaction(
      std::shared_ptr<const MountingCoordinator> /*mountingCoordinator*/,
      bool /*mountSynchronously*/) const override {}

  void shadowTreeDidFinishReactCommit(
      const ShadowTree& /*shadowTree*/) const override {}
};

} // namespace

TEST(ThreadLocalTelemetryTest, aNestedCommitRestoresTheOuterTelemetry) {
  ContextContainer contextContainer{};
  WillCommitShadowTreeDelegate delegate;
  ShadowTree outerTree{
      SurfaceId{1},
      LayoutConstraints{},
      LayoutContext{},
      delegate,
      contextContainer};
  ShadowTree nestedTree{
      SurfaceId{2},
      LayoutConstraints{},
      LayoutContext{},
      delegate,
      contextContainer};
  auto commit = [](const ShadowTree& shadowTree) {
    shadowTree.commit(
        [](const RootShadowNode& oldRootShadowNode) {
          return std::static_pointer_cast<RootShadowNode>(
              oldRootShadowNode.ShadowNode::clone({}));
        },
        {.enableStateReconciliation = false});
  };
  TransactionTelemetry* outerTelemetry = nullptr;
  TransactionTelemetry* telemetryAfterNestedCommit = nullptr;
  delegate.willCommit = [&]() {
    outerTelemetry = TransactionTelemetry::threadLocalTelemetry();
    commit(nestedTree);
    telemetryAfterNestedCommit = TransactionTelemetry::threadLocalTelemetry();
  };

  commit(outerTree);

  EXPECT_NE(outerTelemetry, nullptr);
  EXPECT_EQ(telemetryAfterNestedCommit, outerTelemetry);
  EXPECT_EQ(TransactionTelemetry::threadLocalTelemetry(), nullptr);
}
