/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/PrivateGuard.h>

#include <React/Debug.h>
#include <React/RendererCore.h>
#include <React/View.h>
#include <folly/dynamic.h>
#include <react/renderer/mounting/MountingOverrideDelegate.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/uimanager/UIManagerCommitHook.h>
#include "AnimatedProps.h"
#include "AnimationMutation.h"

namespace facebook::react {

struct PropsSnapshot {
  std::unique_ptr<BaseViewProps> props;
  std::unordered_set<PropName> propNames;
  // The edges that typed margin, padding, position and border width set.
  std::unordered_map<PropName, std::unordered_set<yoga::Edge>> layoutEdges;
  std::unique_ptr<folly::dynamic> rawProps;
};

struct RecordedWrite {
  uint64_t number;
  folly::dynamic value;
  // The ownership connection that wrote an owned key, 0 for others.
  uint64_t connection;
};

// The final values of a view's owned props whose animation finished, which
// the next end sync commits.
struct EndSyncValues {
  std::weak_ptr<const ShadowNodeFamily> family;
  std::unordered_map<std::string, RecordedWrite> values;
};

struct SurfaceContext {
  std::unordered_map<Tag, std::unique_ptr<PropsSnapshot>> pendingMap, map;
  std::unordered_set<std::shared_ptr<const ShadowNodeFamily>> pendingFamilies, families;
  std::unordered_map<Tag, EndSyncValues> endSyncValues;
  // The write number of the last end sync snapshot.
  uint64_t endSyncNumber{0};
  // The release coverage of the newest transaction pulled for the surface.
  uint64_t pulledReleaseCoverage{0};
  // The views released with written owned props after the pulled release
  // coverage, with their release numbers, in release order.
  std::vector<std::pair<uint64_t, std::weak_ptr<const ShadowNodeFamily>>> releasedFamilies;
};

using SnapshotMap = std::unordered_map<Tag, std::unique_ptr<PropsSnapshot>>;

// The animated props a React commit stamps, and the write number they cover:
// every write up to it.
struct StampingSnapshot {
  uint64_t number{0};
  std::unordered_set<std::shared_ptr<const ShadowNodeFamily>> families;
  std::unordered_map<Tag, PropsSnapshot> props;
};

// The props the backend last wrote straight to a view, each with the number
// of the frame that wrote it.
struct DirectWrites {
  std::weak_ptr<const ShadowNodeFamily> family;
  std::unordered_map<std::string, RecordedWrite> rawProps;
  std::unordered_map<PropName, uint64_t> typedPropNumbers;
  std::unique_ptr<BaseViewProps> typedProps;
  // Owned key -> the connection that owns it.
  std::unordered_map<std::string, uint64_t> ownedKeys;
};

// A C++ Animated props node owning keys of a view, from `own` until released.
struct OwnershipConnection {
  SurfaceId surfaceId;
  std::weak_ptr<const ShadowNodeFamily> family;
  Tag propsNodeTag;
  // The release sequence number, 0 while owned.
  uint64_t releasedAt{0};
};

/*
 * Merges raw animated props per key, like the props merged by the animation
 * drivers. A null value is kept, so it still resets the prop on commit.
 */
void mergeAnimatedRawProps(folly::dynamic &target, const folly::dynamic &source);

#ifdef RN_SERIALIZABLE_STATE
// The values `newProps` gives the keys of `assumedNative`, where they differ
// from the values the native view is assumed to show.
folly::dynamic restoreValues(
    SurfaceId surfaceId,
    const ComponentDescriptor &componentDescriptor,
    const Props::Shared &newProps,
    const folly::dynamic &assumedNative);
#endif

// A mount of a revision stamped before a direct write keeps the written
// props: a direct write wins when its number is greater than the stamp. An
// owned key's write wins until a revision covering its release is mounted.
class AnimatedPropsRegistry : public MountingTransactionFinalizer {
 public:
  // Records a frame's mutations, merged per view, and returns the frame's
  // write number.
  uint64_t update(const std::unordered_map<SurfaceId, std::unordered_map<Tag, AnimationMutation>> &surfaceUpdates);
  void initializeSurface(SurfaceId surfaceId);
  void clear(SurfaceId surfaceId);
  void clearOnSurfaceStop(SurfaceId surfaceId);
  // With `endSync`, the snapshot includes the finished owned values.
  StampingSnapshot takeStampingSnapshot(SurfaceId surfaceId, bool endSync);

  void
  own(const std::shared_ptr<const ShadowNodeFamily> &family, Tag propsNodeTag, const std::vector<std::string> &keys);
  // Ends the props node's connection, returning it for its release.
  std::optional<uint64_t> disconnect(Tag propsNodeTag);
  std::vector<SurfaceId> surfacesOf(const std::vector<uint64_t> &connections) const;
  // Releases the keys the connections own on a surface at a new release
  // sequence number, which it returns with the families of the views whose
  // released keys were written.
  std::pair<uint64_t, std::vector<std::shared_ptr<const ShadowNodeFamily>>> releaseOnSurface(
      SurfaceId surfaceId,
      const std::vector<uint64_t> &connections);
  // Removes the props of the view's owned keys, raw and typed.
  void removeOwnedProps(SurfaceId surfaceId, Tag tag, AnimatedProps &props) const;

  std::vector<folly::dynamic> newerDirectWrites(SurfaceId surfaceId, const std::vector<StampedView> &stampedViews)
      const;
  bool hasDirectWrites(SurfaceId surfaceId) const;
#ifdef RN_SERIALIZABLE_STATE
  folly::dynamic restoreDirectWrites(
      SurfaceId surfaceId,
      Tag tag,
      const Props::Shared &props,
      const Props::Shared &preallocatedProps) const;
  // The values `props` gives the keys whose owning connection was released
  // after `afterReleaseCoverage` and up to `releaseCoverage`.
  folly::dynamic releasedValues(
      SurfaceId surfaceId,
      Tag tag,
      uint64_t afterReleaseCoverage,
      uint64_t releaseCoverage,
      const Props::Shared &props) const;
#endif

  MountingTransaction finalizeTransaction(MountingTransaction transaction, const ShadowNode &root) const override;

 private:
  friend class AnimatedPropsRegistryTest;

  // Records owned typed values as raw values, with the connection that owns
  // them.
  void recordDirectWrite(
      const AnimationMutation &mutation,
      const folly::dynamic *rawProps,
      const folly::dynamic &ownedTypedValues);
  bool isConnected(uint64_t connection) const;
  // Erases the connection unless its node is connected or it owns or wrote a
  // key of its view.
  void eraseIfUnreferenced(uint64_t connection);
  // The view's owned keys and their connections.
  std::unordered_map<std::string, uint64_t> ownedKeys(SurfaceId surfaceId, Tag tag) const;
  // Erases the keys' values from the stamping snapshots and the end sync
  // values of the view.
  void eraseOwnedKeys(SurfaceContext &surfaceContext, Tag tag, const std::vector<std::string> &keys);
  // The values of the view's writes that win against a mount of `group`;
  // all recorded values without one.
  folly::dynamic newerDirectWrites(const DirectWrites &view, const std::optional<StampedView> &group) const;
  bool wins(const RecordedWrite &write, const StampedView &group) const;
  void overlayNewerDirectWrites(SurfaceId surfaceId, ShadowViewMutationList &mutations, const StampedView &group) const;

  // Mutable for the pulled release coverage the finalizer tracks.
  mutable std::unordered_map<SurfaceId, SurfaceContext> surfaceContexts_;
  std::unordered_map<SurfaceId, std::unordered_map<Tag, DirectWrites>> directWrites_;
  std::unordered_map<uint64_t, OwnershipConnection> connections_;
  std::unordered_map<Tag, uint64_t> nodeConnections_;
  uint64_t writeNumber_{0};
  uint64_t connectionNumber_{0};
  uint64_t releaseNumber_{0};
  mutable std::mutex mutex_;
};

inline void updateLayoutEdge(PropName propName, yoga::Edge edge, BaseViewProps &viewProps, const BaseViewProps &source)
{
  switch (propName) {
    case MARGIN:
      viewProps.yogaStyle.setMargin(edge, source.yogaStyle.margin(edge));
      break;
    case PADDING:
      viewProps.yogaStyle.setPadding(edge, source.yogaStyle.padding(edge));
      break;
    case POSITION:
      viewProps.yogaStyle.setPosition(edge, source.yogaStyle.position(edge));
      break;
    case BORDER_WIDTH:
      viewProps.yogaStyle.setBorder(edge, source.yogaStyle.border(edge));
      break;
    default:
      break;
  }
}

inline void updateProp(const PropName propName, BaseViewProps &viewProps, const PropsSnapshot &snapshot)
{
  react_native_assert(snapshot.propNames.contains(propName));
  if (!snapshot.props) {
    return;
  }
  switch (propName) {
    case OPACITY:
      viewProps.opacity = snapshot.props->opacity;
      break;

    case WIDTH:
      viewProps.yogaStyle.setDimension(
          yoga::Dimension::Width, snapshot.props->yogaStyle.dimension(yoga::Dimension::Width));
      break;

    case HEIGHT: {
      auto d = snapshot.props->yogaStyle.dimension(yoga::Dimension::Height);
      viewProps.yogaStyle.setDimension(yoga::Dimension::Height, d);
      break;
    }

    case TRANSFORM:
      viewProps.transform = snapshot.props->transform;
      break;

    case TRANSFORM_ORIGIN:
      viewProps.transformOrigin = snapshot.props->transformOrigin;
      break;

    case BORDER_RADII:
      viewProps.borderRadii = snapshot.props->borderRadii;
      break;

    case FLEX:
      viewProps.yogaStyle.setFlex(snapshot.props->yogaStyle.flex());
      break;

    case BACKGROUND_COLOR:
      viewProps.backgroundColor = snapshot.props->backgroundColor;
      break;

    case SHADOW_COLOR:
      viewProps.shadowColor = snapshot.props->shadowColor;
      break;

    case SHADOW_OFFSET:
      viewProps.shadowOffset = snapshot.props->shadowOffset;
      break;

    case SHADOW_OPACITY:
      viewProps.shadowOpacity = snapshot.props->shadowOpacity;
      break;

    case SHADOW_RADIUS:
      viewProps.shadowRadius = snapshot.props->shadowRadius;
      break;

    case MARGIN:
      viewProps.yogaStyle.setMargin(yoga::Edge::Left, snapshot.props->yogaStyle.margin(yoga::Edge::Left));
      viewProps.yogaStyle.setMargin(yoga::Edge::Right, snapshot.props->yogaStyle.margin(yoga::Edge::Right));
      viewProps.yogaStyle.setMargin(yoga::Edge::Top, snapshot.props->yogaStyle.margin(yoga::Edge::Top));
      viewProps.yogaStyle.setMargin(yoga::Edge::Bottom, snapshot.props->yogaStyle.margin(yoga::Edge::Bottom));
      viewProps.yogaStyle.setMargin(yoga::Edge::Start, snapshot.props->yogaStyle.margin(yoga::Edge::Start));
      viewProps.yogaStyle.setMargin(yoga::Edge::End, snapshot.props->yogaStyle.margin(yoga::Edge::End));
      viewProps.yogaStyle.setMargin(yoga::Edge::Horizontal, snapshot.props->yogaStyle.margin(yoga::Edge::Horizontal));
      viewProps.yogaStyle.setMargin(yoga::Edge::Vertical, snapshot.props->yogaStyle.margin(yoga::Edge::Vertical));
      break;

    case PADDING:
      viewProps.yogaStyle.setPadding(yoga::Edge::Left, snapshot.props->yogaStyle.padding(yoga::Edge::Left));
      viewProps.yogaStyle.setPadding(yoga::Edge::Right, snapshot.props->yogaStyle.padding(yoga::Edge::Right));
      viewProps.yogaStyle.setPadding(yoga::Edge::Top, snapshot.props->yogaStyle.padding(yoga::Edge::Top));
      viewProps.yogaStyle.setPadding(yoga::Edge::Bottom, snapshot.props->yogaStyle.padding(yoga::Edge::Bottom));
      viewProps.yogaStyle.setPadding(yoga::Edge::Start, snapshot.props->yogaStyle.padding(yoga::Edge::Start));
      viewProps.yogaStyle.setPadding(yoga::Edge::End, snapshot.props->yogaStyle.padding(yoga::Edge::End));
      viewProps.yogaStyle.setPadding(yoga::Edge::Horizontal, snapshot.props->yogaStyle.padding(yoga::Edge::Horizontal));
      viewProps.yogaStyle.setPadding(yoga::Edge::Vertical, snapshot.props->yogaStyle.padding(yoga::Edge::Vertical));
      break;

    case POSITION:
      viewProps.yogaStyle.setPosition(yoga::Edge::Left, snapshot.props->yogaStyle.position(yoga::Edge::Left));
      viewProps.yogaStyle.setPosition(yoga::Edge::Right, snapshot.props->yogaStyle.position(yoga::Edge::Right));
      viewProps.yogaStyle.setPosition(yoga::Edge::Top, snapshot.props->yogaStyle.position(yoga::Edge::Top));
      viewProps.yogaStyle.setPosition(yoga::Edge::Bottom, snapshot.props->yogaStyle.position(yoga::Edge::Bottom));
      viewProps.yogaStyle.setPosition(yoga::Edge::Start, snapshot.props->yogaStyle.position(yoga::Edge::Start));
      viewProps.yogaStyle.setPosition(yoga::Edge::End, snapshot.props->yogaStyle.position(yoga::Edge::End));
      viewProps.yogaStyle.setPosition(
          yoga::Edge::Horizontal, snapshot.props->yogaStyle.position(yoga::Edge::Horizontal));
      viewProps.yogaStyle.setPosition(yoga::Edge::Vertical, snapshot.props->yogaStyle.position(yoga::Edge::Vertical));
      break;

    case BORDER_WIDTH:
      viewProps.yogaStyle.setBorder(yoga::Edge::Left, snapshot.props->yogaStyle.border(yoga::Edge::Left));
      viewProps.yogaStyle.setBorder(yoga::Edge::Right, snapshot.props->yogaStyle.border(yoga::Edge::Right));
      viewProps.yogaStyle.setBorder(yoga::Edge::Top, snapshot.props->yogaStyle.border(yoga::Edge::Top));
      viewProps.yogaStyle.setBorder(yoga::Edge::Bottom, snapshot.props->yogaStyle.border(yoga::Edge::Bottom));
      viewProps.yogaStyle.setBorder(yoga::Edge::Start, snapshot.props->yogaStyle.border(yoga::Edge::Start));
      viewProps.yogaStyle.setBorder(yoga::Edge::End, snapshot.props->yogaStyle.border(yoga::Edge::End));
      viewProps.yogaStyle.setBorder(yoga::Edge::Horizontal, snapshot.props->yogaStyle.border(yoga::Edge::Horizontal));
      viewProps.yogaStyle.setBorder(yoga::Edge::Vertical, snapshot.props->yogaStyle.border(yoga::Edge::Vertical));
      break;

    case BORDER_COLOR:
      viewProps.borderColors = snapshot.props->borderColors;
      break;

    case FILTER:
      viewProps.filter = snapshot.props->filter;
      break;

    case OUTLINE_COLOR:
      viewProps.outlineColor = snapshot.props->outlineColor;
      break;

    case OUTLINE_OFFSET:
      viewProps.outlineOffset = snapshot.props->outlineOffset;
      break;

    case OUTLINE_STYLE:
      viewProps.outlineStyle = snapshot.props->outlineStyle;
      break;

    case OUTLINE_WIDTH:
      viewProps.outlineWidth = snapshot.props->outlineWidth;
      break;

    case ALIGN_CONTENT:
      viewProps.yogaStyle.setAlignContent(snapshot.props->yogaStyle.alignContent());
      break;

    case ALIGN_ITEMS:
      viewProps.yogaStyle.setAlignItems(snapshot.props->yogaStyle.alignItems());
      break;

    case ALIGN_SELF:
      viewProps.yogaStyle.setAlignSelf(snapshot.props->yogaStyle.alignSelf());
      break;

    case ASPECT_RATIO:
      viewProps.yogaStyle.setAspectRatio(snapshot.props->yogaStyle.aspectRatio());
      break;

    case BOX_SIZING:
      viewProps.yogaStyle.setBoxSizing(snapshot.props->yogaStyle.boxSizing());
      break;

    case DISPLAY:
      viewProps.yogaStyle.setDisplay(snapshot.props->yogaStyle.display());
      break;

    case FLEX_BASIS:
      viewProps.yogaStyle.setFlexBasis(snapshot.props->yogaStyle.flexBasis());
      break;

    case FLEX_DIRECTION:
      viewProps.yogaStyle.setFlexDirection(snapshot.props->yogaStyle.flexDirection());
      break;

    case ROW_GAP:
      viewProps.yogaStyle.setGap(yoga::Gutter::Row, snapshot.props->yogaStyle.gap(yoga::Gutter::Row));
      break;

    case COLUMN_GAP:
      viewProps.yogaStyle.setGap(yoga::Gutter::Column, snapshot.props->yogaStyle.gap(yoga::Gutter::Column));
      break;

    case FLEX_GROW:
      viewProps.yogaStyle.setFlexGrow(snapshot.props->yogaStyle.flexGrow());
      break;

    case FLEX_SHRINK:
      viewProps.yogaStyle.setFlexShrink(snapshot.props->yogaStyle.flexShrink());
      break;

    case FLEX_WRAP:
      viewProps.yogaStyle.setFlexWrap(snapshot.props->yogaStyle.flexWrap());
      break;

    case JUSTIFY_CONTENT:
      viewProps.yogaStyle.setJustifyContent(snapshot.props->yogaStyle.justifyContent());
      break;

    case MAX_HEIGHT:
      viewProps.yogaStyle.setMaxDimension(
          yoga::Dimension::Height, snapshot.props->yogaStyle.maxDimension(yoga::Dimension::Height));
      break;

    case MAX_WIDTH:
      viewProps.yogaStyle.setMaxDimension(
          yoga::Dimension::Width, snapshot.props->yogaStyle.maxDimension(yoga::Dimension::Width));
      break;

    case MIN_HEIGHT:
      viewProps.yogaStyle.setMinDimension(
          yoga::Dimension::Height, snapshot.props->yogaStyle.minDimension(yoga::Dimension::Height));
      break;

    case MIN_WIDTH:
      viewProps.yogaStyle.setMinDimension(
          yoga::Dimension::Width, snapshot.props->yogaStyle.minDimension(yoga::Dimension::Width));
      break;

    case STYLE_OVERFLOW:
      viewProps.yogaStyle.setOverflow(snapshot.props->yogaStyle.overflow());
      break;

    case POSITION_TYPE:
      viewProps.yogaStyle.setPositionType(snapshot.props->yogaStyle.positionType());
      break;

    case Z_INDEX:
      viewProps.zIndex = snapshot.props->zIndex;
      break;

    case DIRECTION:
      viewProps.yogaStyle.setDirection(snapshot.props->yogaStyle.direction());
      break;

    case BORDER_CURVES:
      viewProps.borderCurves = snapshot.props->borderCurves;
      break;

    case BORDER_STYLES:
      viewProps.borderStyles = snapshot.props->borderStyles;
      break;

    case POINTER_EVENTS:
      viewProps.pointerEvents = snapshot.props->pointerEvents;
      break;

    case ISOLATION:
      viewProps.isolation = snapshot.props->isolation;
      break;

    case CURSOR:
      viewProps.cursor = snapshot.props->cursor;
      break;

    case BOX_SHADOW:
      viewProps.boxShadow = snapshot.props->boxShadow;
      break;

    case MIX_BLEND_MODE:
      viewProps.mixBlendMode = snapshot.props->mixBlendMode;
      break;

    case BACKFACE_VISIBILITY:
      viewProps.backfaceVisibility = snapshot.props->backfaceVisibility;
      break;

    default:
      throw std::runtime_error("Unknown property name");
  }
}

} // namespace facebook::react
