/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnimatedPropsRegistry.h"
#include <react/debug/react_native_assert.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/mounting/Differentiator.h>
#include <algorithm>
#include <array>
#include "AnimatedProps.h"
#include "AnimatedPropsSerializer.h"

namespace facebook::react {

void mergeAnimatedRawProps(
    folly::dynamic& target,
    const folly::dynamic& source) {
  if (!target.isObject() || !source.isObject()) {
    target = source;
    return;
  }
  for (const auto& [key, value] : source.items()) {
    target[key] = value;
  }
}

namespace {

void mergeRawProps(
    std::unique_ptr<folly::dynamic>& target,
    const folly::dynamic& source) {
  if (target) {
    mergeAnimatedRawProps(*target, source);
  } else {
    target = std::make_unique<folly::dynamic>(source);
  }
}

// The raw props key that sets the same value as a typed prop. Typed props set
// per edge or corner are not retained as typed props, and layout ones are
// keyed by edge.
const std::vector<std::string>& rawPropKeys(PropName propName) {
  static const std::unordered_map<PropName, std::vector<std::string>> keys{
      {OPACITY, {"opacity"}},
      {WIDTH, {"width"}},
      {HEIGHT, {"height"}},
      {FLEX, {"flex"}},
      {TRANSFORM, {"transform"}},
      {TRANSFORM_ORIGIN, {"transformOrigin"}},
      {BACKGROUND_COLOR, {"backgroundColor"}},
      {SHADOW_COLOR, {"shadowColor"}},
      {SHADOW_OFFSET, {"shadowOffset"}},
      {SHADOW_OPACITY, {"shadowOpacity"}},
      {SHADOW_RADIUS, {"shadowRadius"}},
      {FILTER, {"filter"}},
      {OUTLINE_COLOR, {"outlineColor"}},
      {OUTLINE_OFFSET, {"outlineOffset"}},
      {OUTLINE_STYLE, {"outlineStyle"}},
      {OUTLINE_WIDTH, {"outlineWidth"}},
      {ALIGN_CONTENT, {"alignContent"}},
      {ALIGN_ITEMS, {"alignItems"}},
      {ALIGN_SELF, {"alignSelf"}},
      {ASPECT_RATIO, {"aspectRatio"}},
      {BOX_SIZING, {"boxSizing"}},
      {DISPLAY, {"display"}},
      {FLEX_BASIS, {"flexBasis"}},
      {FLEX_DIRECTION, {"flexDirection"}},
      {ROW_GAP, {"rowGap"}},
      {COLUMN_GAP, {"columnGap"}},
      {FLEX_GROW, {"flexGrow"}},
      {FLEX_SHRINK, {"flexShrink"}},
      {FLEX_WRAP, {"flexWrap"}},
      {JUSTIFY_CONTENT, {"justifyContent"}},
      {MAX_HEIGHT, {"maxHeight"}},
      {MAX_WIDTH, {"maxWidth"}},
      {MIN_HEIGHT, {"minHeight"}},
      {MIN_WIDTH, {"minWidth"}},
      {STYLE_OVERFLOW, {"overflow"}},
      {POSITION_TYPE, {"position"}},
      {Z_INDEX, {"zIndex"}},
      {DIRECTION, {"direction"}},
      {POINTER_EVENTS, {"pointerEvents"}},
      {ISOLATION, {"isolation"}},
      {CURSOR, {"cursor"}},
      {BOX_SHADOW, {"boxShadow"}},
      {MIX_BLEND_MODE, {"mixBlendMode"}},
      {BACKFACE_VISIBILITY, {"backfaceVisibility"}},
  };
  static const std::vector<std::string> none;
  auto it = keys.find(propName);
  return it != keys.end() ? it->second : none;
}

std::optional<PropName> typedPropSetBy(const std::string& rawKey) {
  static const auto typedProps = [] {
    std::unordered_map<std::string, PropName> typedProps;
    for (int propName = OPACITY; propName <= BACKFACE_VISIBILITY; propName++) {
      for (const auto& key : rawPropKeys(static_cast<PropName>(propName))) {
        typedProps.emplace(key, static_cast<PropName>(propName));
      }
    }
    return typedProps;
  }();
  auto it = typedProps.find(rawKey);
  return it != typedProps.end() ? std::optional{it->second} : std::nullopt;
}

constexpr std::array<yoga::Edge, 9> kLayoutEdges{
    yoga::Edge::Left,
    yoga::Edge::Top,
    yoga::Edge::Right,
    yoga::Edge::Bottom,
    yoga::Edge::Start,
    yoga::Edge::End,
    yoga::Edge::Horizontal,
    yoga::Edge::Vertical,
    yoga::Edge::All};

std::string layoutEdgeKey(PropName propName, yoga::Edge edge) {
  static const std::unordered_map<yoga::Edge, std::string> edgeNames{
      {yoga::Edge::Left, "Left"},
      {yoga::Edge::Top, "Top"},
      {yoga::Edge::Right, "Right"},
      {yoga::Edge::Bottom, "Bottom"},
      {yoga::Edge::Start, "Start"},
      {yoga::Edge::End, "End"},
      {yoga::Edge::Horizontal, "Horizontal"},
      {yoga::Edge::Vertical, "Vertical"},
      {yoga::Edge::All, ""}};
  static const std::unordered_map<yoga::Edge, std::string> positionKeys{
      {yoga::Edge::Left, "left"},
      {yoga::Edge::Top, "top"},
      {yoga::Edge::Right, "right"},
      {yoga::Edge::Bottom, "bottom"},
      {yoga::Edge::Start, "start"},
      {yoga::Edge::End, "end"},
      {yoga::Edge::Horizontal, "insetInline"},
      {yoga::Edge::Vertical, "insetBlock"},
      {yoga::Edge::All, "inset"}};
  switch (propName) {
    case MARGIN:
      return "margin" + edgeNames.at(edge);
    case PADDING:
      return "padding" + edgeNames.at(edge);
    case BORDER_WIDTH:
      return "border" + edgeNames.at(edge) + "Width";
    default:
      return positionKeys.at(edge);
  }
}

bool isLayoutEdgeProp(PropName propName) {
  return propName == MARGIN || propName == PADDING || propName == POSITION ||
      propName == BORDER_WIDTH;
}

std::optional<std::pair<PropName, yoga::Edge>> layoutEdgeSetBy(
    const std::string& rawKey) {
  static const auto layoutEdges = [] {
    std::unordered_map<std::string, std::pair<PropName, yoga::Edge>> edges;
    for (auto propName : {MARGIN, PADDING, POSITION, BORDER_WIDTH}) {
      for (auto edge : kLayoutEdges) {
        edges.emplace(layoutEdgeKey(propName, edge), std::pair{propName, edge});
      }
    }
    return edges;
  }();
  auto it = layoutEdges.find(rawKey);
  return it != layoutEdges.end() ? std::optional{it->second} : std::nullopt;
}

std::vector<yoga::Edge> setLayoutEdges(const AnimatedPropBase& animatedProp) {
  const auto& edges =
      get<CascadedRectangleEdges<yoga::StyleLength>>(animatedProp);
  std::vector<yoga::Edge> setEdges;
  auto add = [&](yoga::Edge edge, const auto& value) {
    if (value.has_value()) {
      setEdges.push_back(edge);
    }
  };
  add(yoga::Edge::Left, edges.left);
  add(yoga::Edge::Top, edges.top);
  add(yoga::Edge::Right, edges.right);
  add(yoga::Edge::Bottom, edges.bottom);
  add(yoga::Edge::Start, edges.start);
  add(yoga::Edge::End, edges.end);
  add(yoga::Edge::Horizontal, edges.horizontal);
  add(yoga::Edge::Vertical, edges.vertical);
  add(yoga::Edge::All, edges.all);
  return setEdges;
}

void supersedeTypedProps(
    PropsSnapshot& snapshot,
    const folly::dynamic& rawProps) {
  for (const auto& [key, _] : rawProps.items()) {
    if (auto propName = typedPropSetBy(key.getString())) {
      snapshot.propNames.erase(*propName);
    } else if (auto layoutEdge = layoutEdgeSetBy(key.getString())) {
      auto& [edgePropName, edge] = *layoutEdge;
      if (auto it = snapshot.layoutEdges.find(edgePropName);
          it != snapshot.layoutEdges.end()) {
        it->second.erase(edge);
        if (it->second.empty()) {
          snapshot.layoutEdges.erase(it);
          snapshot.propNames.erase(edgePropName);
        }
      }
    }
  }
}

void supersedeRawProps(
    PropsSnapshot& snapshot,
    PropName propName,
    const std::unordered_set<yoga::Edge>& edges) {
  if (!snapshot.rawProps) {
    return;
  }
  if (!isLayoutEdgeProp(propName)) {
    for (const auto& key : rawPropKeys(propName)) {
      snapshot.rawProps->erase(key);
    }
    return;
  }
  for (auto edge : edges) {
    snapshot.rawProps->erase(layoutEdgeKey(propName, edge));
  }
}

// Copies the typed prop's value, only the edges it sets for layout edges.
void copyTypedProp(
    PropName propName,
    PropsSnapshot& target,
    const PropsSnapshot& source) {
  target.propNames.insert(propName);
  auto edgesIt = source.layoutEdges.find(propName);
  if (edgesIt == source.layoutEdges.end()) {
    updateProp(propName, *target.props, source);
    return;
  }
  for (auto edge : edgesIt->second) {
    target.layoutEdges[propName].insert(edge);
    updateLayoutEdge(propName, edge, *target.props, *source.props);
  }
}

bool ownsTypedProp(
    PropName propName,
    const std::unordered_map<std::string, uint64_t>& ownedKeys) {
  const auto& keys = rawPropKeys(propName);
  return std::any_of(keys.begin(), keys.end(), [&](const auto& key) {
    return ownedKeys.contains(key);
  });
}

Props::Shared cloneProps(
    SurfaceId surfaceId,
    const ShadowNodeFamily& family,
    const Props::Shared& props) {
  const auto& componentDescriptor = family.getComponentDescriptor();
  PropsParserContext propsParserContext{
      surfaceId, *componentDescriptor.getContextContainer()};
  return componentDescriptor.cloneProps(propsParserContext, props, {});
}

// Makes each released view the transaction leaves mounted apply its props
// again, so React's values replace the owned ones.
void updateReleasedViews(
    SurfaceId surfaceId,
    ShadowViewMutationList& mutations,
    const ShadowNode& root,
    const std::vector<std::shared_ptr<const ShadowNodeFamily>>& families) {
  struct ReleasedView {
    const ShadowNodeFamily* family;
    bool appliesProps{false};
    bool isDeleted{false};
    ShadowViewMutation* layoutUpdate{nullptr};
  };
  std::unordered_map<Tag, ReleasedView> views;
  for (const auto& family : families) {
    views.emplace(family->getTag(), ReleasedView{.family = family.get()});
  }
  for (auto& mutation : mutations) {
    if (mutation.type == ShadowViewMutation::Delete) {
      if (auto it = views.find(mutation.oldChildShadowView.tag);
          it != views.end()) {
        it->second.isDeleted = true;
      }
      continue;
    }
    auto it = views.find(mutation.newChildShadowView.tag);
    if (it == views.end()) {
      continue;
    }
    auto& view = it->second;
    if (mutation.type == ShadowViewMutation::Create) {
      view.isDeleted = false;
    }
    if (mutation.type == ShadowViewMutation::Create ||
        mutation.type == ShadowViewMutation::Insert ||
        (mutation.type == ShadowViewMutation::Update &&
         mutation.oldChildShadowView.props !=
             mutation.newChildShadowView.props)) {
      view.appliesProps = true;
    } else if (mutation.type == ShadowViewMutation::Update) {
      view.layoutUpdate = &mutation;
    }
  }
  ShadowViewMutationList updates;
  for (auto& [_, view] : views) {
    if (view.appliesProps || view.isDeleted) {
      continue;
    }
    if (view.layoutUpdate != nullptr) {
      auto& props = view.layoutUpdate->newChildShadowView.props;
      props = cloneProps(surfaceId, *view.family, props);
      continue;
    }
    if (auto mounted = findMountedShadowView(root, *view.family)) {
      auto newView = mounted->shadowView;
      newView.props = cloneProps(surfaceId, *view.family, newView.props);
      updates.push_back(
          ShadowViewMutation::UpdateMutation(
              mounted->shadowView, newView, mounted->parentTag));
    }
  }
  mutations.insert(
      mutations.end(),
      std::make_move_iterator(updates.begin()),
      std::make_move_iterator(updates.end()));
}

} // namespace

uint64_t AnimatedPropsRegistry::update(
    const std::unordered_map<
        SurfaceId,
        std::unordered_map<Tag, AnimationMutation>>& surfaceUpdates) {
  auto lock = std::lock_guard(mutex_);
  writeNumber_++;
  for (const auto& [_, updates] : surfaceUpdates) {
    for (const auto& [__, mutation] : updates) {
      const auto& family = mutation.family;
      react_native_assert(family != nullptr);
      auto contextIt = surfaceContexts_.find(family->getSurfaceId());
      if (contextIt == surfaceContexts_.end()) {
        continue;
      }
      const auto& animatedProps = mutation.props;
      folly::dynamic convertedRawProps;
      const folly::dynamic* rawProps = nullptr;
      if (animatedProps.rawProps) {
        rawProps = animatedProps.rawProps->getDynamic();
        if (rawProps == nullptr) {
          convertedRawProps = animatedProps.rawProps->toDynamic();
          rawProps = &convertedRawProps;
        }
      }
      auto& surfaceContext = contextIt->second;
      auto owned = ownedKeys(family->getSurfaceId(), mutation.tag);
      folly::dynamic ownedTypedValues = folly::dynamic::object();
      for (const auto& animatedProp : animatedProps.props) {
        if (ownsTypedProp(animatedProp->propName, owned)) {
          animationbackend::packAnimatedProp(ownedTypedValues, animatedProp);
        }
      }
      recordDirectWrite(mutation, rawProps, ownedTypedValues);

      if (!owned.empty()) {
        // Owned values reach React commits only when their animation
        // finished, through the end sync.
        for (const auto& [key, connection] : owned) {
          // Typed values are written after raw ones.
          const auto* value = ownedTypedValues.get_ptr(key);
          if (value == nullptr && rawProps != nullptr) {
            value = rawProps->get_ptr(key);
          }
          if (value != nullptr && mutation.finishedKeys.contains(key)) {
            auto& endSyncValues = surfaceContext.endSyncValues[mutation.tag];
            endSyncValues.family = family;
            endSyncValues.values[key] = {
                .number = writeNumber_,
                .value = *value,
                .connection = connection};
          }
        }
        if (rawProps != nullptr) {
          auto unownedRawProps = *rawProps;
          for (const auto& [key, _] : owned) {
            unownedRawProps.erase(key);
          }
          convertedRawProps = std::move(unownedRawProps);
          rawProps = &convertedRawProps;
        }
      }
      bool stampsRawProps = rawProps != nullptr && !rawProps->empty();
      bool stampsTypedProps = std::any_of(
          animatedProps.props.begin(),
          animatedProps.props.end(),
          [&](const auto& animatedProp) {
            return !ownsTypedProp(animatedProp->propName, owned);
          });
      if (!stampsRawProps && !stampsTypedProps) {
        continue;
      }
      surfaceContext.pendingFamilies.insert(family);
      auto& snapshot = surfaceContext.pendingMap[mutation.tag];
      if (!snapshot) {
        snapshot = std::make_unique<PropsSnapshot>();
      }
      if (stampsRawProps) {
        supersedeTypedProps(*snapshot, *rawProps);
        mergeRawProps(snapshot->rawProps, *rawProps);
      }
      if (stampsTypedProps && !snapshot->props) {
        snapshot->props = std::make_unique<BaseViewProps>();
      }
      for (const auto& animatedProp : animatedProps.props) {
        auto propName = animatedProp->propName;
        if (ownsTypedProp(propName, owned)) {
          continue;
        }
        std::unordered_set<yoga::Edge> edges;
        if (isLayoutEdgeProp(propName)) {
          auto setEdges = setLayoutEdges(*animatedProp);
          if (setEdges.empty()) {
            continue;
          }
          edges.insert(setEdges.begin(), setEdges.end());
          snapshot->layoutEdges[propName].insert(edges.begin(), edges.end());
        }
        supersedeRawProps(*snapshot, propName, edges);
        snapshot->propNames.insert(propName);
        cloneProp(*snapshot->props, *animatedProp);
      }
    }
  }
  return writeNumber_;
}

void AnimatedPropsRegistry::recordDirectWrite(
    const AnimationMutation& mutation,
    const folly::dynamic* rawProps,
    const folly::dynamic& ownedTypedValues) {
  bool writesDirectly = (rawProps != nullptr && !rawProps->empty()) ||
      std::any_of(mutation.props.props.begin(),
                  mutation.props.props.end(),
                  [](const auto& animatedProp) {
                    return animationbackend::isWrittenDirectly(
                        animatedProp->propName);
                  });
  if (!writesDirectly) {
    return;
  }
  auto& view = directWrites_[mutation.family->getSurfaceId()][mutation.tag];
  view.family = mutation.family;
  std::vector<uint64_t> replacedConnections;
  auto recordRawProps = [&](const folly::dynamic& values) {
    for (const auto& [key, value] : values.items()) {
      auto ownerIt = view.ownedKeys.find(key.getString());
      auto connection = ownerIt != view.ownedKeys.end() ? ownerIt->second : 0;
      auto& write = view.rawProps[key.getString()];
      if (write.connection != connection) {
        replacedConnections.push_back(write.connection);
      }
      write = {
          .number = writeNumber_, .value = value, .connection = connection};
    }
  };
  if (rawProps != nullptr) {
    recordRawProps(*rawProps);
  }
  recordRawProps(ownedTypedValues);
  for (auto connection : replacedConnections) {
    eraseIfUnreferenced(connection);
  }
  for (const auto& animatedProp : mutation.props.props) {
    if (!animationbackend::isWrittenDirectly(animatedProp->propName)) {
      continue;
    }
    if (ownsTypedProp(animatedProp->propName, view.ownedKeys)) {
      view.typedPropNumbers.erase(animatedProp->propName);
      continue;
    }
    if (!view.typedProps) {
      view.typedProps = std::make_unique<BaseViewProps>();
    }
    cloneProp(*view.typedProps, *animatedProp);
    view.typedPropNumbers[animatedProp->propName] = writeNumber_;
  }
}

std::unordered_map<std::string, uint64_t> AnimatedPropsRegistry::ownedKeys(
    SurfaceId surfaceId,
    Tag tag) const {
  auto viewsIt = directWrites_.find(surfaceId);
  if (viewsIt == directWrites_.end()) {
    return {};
  }
  auto viewIt = viewsIt->second.find(tag);
  return viewIt != viewsIt->second.end()
      ? viewIt->second.ownedKeys
      : std::unordered_map<std::string, uint64_t>{};
}

void AnimatedPropsRegistry::removeOwnedProps(
    SurfaceId surfaceId,
    Tag tag,
    AnimatedProps& props) const {
  std::unordered_map<std::string, uint64_t> owned;
  {
    auto lock = std::lock_guard(mutex_);
    owned = ownedKeys(surfaceId, tag);
  }
  if (owned.empty()) {
    return;
  }
  if (props.rawProps) {
    auto rawProps = props.rawProps->toDynamic();
    for (const auto& [key, _] : owned) {
      rawProps.erase(key);
    }
    props.rawProps = std::make_unique<RawProps>(std::move(rawProps));
  }
  std::erase_if(props.props, [&](const auto& animatedProp) {
    return ownsTypedProp(animatedProp->propName, owned);
  });
}

void AnimatedPropsRegistry::own(
    const std::shared_ptr<const ShadowNodeFamily>& family,
    Tag propsNodeTag,
    const std::vector<std::string>& keys) {
  auto lock = std::lock_guard(mutex_);
  auto surfaceId = family->getSurfaceId();
  if (!surfaceContexts_.contains(surfaceId)) {
    return;
  }
  auto connection = ++connectionNumber_;
  auto& view = directWrites_[surfaceId][family->getTag()];
  view.family = family;
  std::vector<uint64_t> replacedConnections;
  for (const auto& key : keys) {
    auto& owner = view.ownedKeys[key];
    replacedConnections.push_back(owner);
    owner = connection;
    // The new connection takes over the value a still connected one wrote.
    if (auto writeIt = view.rawProps.find(key);
        writeIt != view.rawProps.end() &&
        isConnected(writeIt->second.connection)) {
      replacedConnections.push_back(writeIt->second.connection);
      writeIt->second.connection = connection;
    }
  }
  if (auto nodeIt = nodeConnections_.find(propsNodeTag);
      nodeIt != nodeConnections_.end()) {
    replacedConnections.push_back(nodeIt->second);
  }
  connections_[connection] = {
      .surfaceId = surfaceId, .family = family, .propsNodeTag = propsNodeTag};
  nodeConnections_[propsNodeTag] = connection;
  eraseOwnedKeys(surfaceContexts_.at(surfaceId), family->getTag(), keys);
  for (auto replacedConnection : replacedConnections) {
    eraseIfUnreferenced(replacedConnection);
  }
}

void AnimatedPropsRegistry::eraseOwnedKeys(
    SurfaceContext& surfaceContext,
    Tag tag,
    const std::vector<std::string>& keys) {
  for (auto* snapshots : {&surfaceContext.pendingMap, &surfaceContext.map}) {
    auto snapshotIt = snapshots->find(tag);
    if (snapshotIt == snapshots->end()) {
      continue;
    }
    auto& snapshot = *snapshotIt->second;
    for (const auto& key : keys) {
      if (snapshot.rawProps) {
        snapshot.rawProps->erase(key);
      }
      if (auto propName = typedPropSetBy(key)) {
        snapshot.propNames.erase(*propName);
      }
    }
    if (snapshot.propNames.empty() &&
        (!snapshot.rawProps || snapshot.rawProps->empty())) {
      snapshots->erase(snapshotIt);
    }
  }
  if (!surfaceContext.pendingMap.contains(tag) &&
      !surfaceContext.map.contains(tag)) {
    for (auto* families :
         {&surfaceContext.pendingFamilies, &surfaceContext.families}) {
      std::erase_if(*families, [&](const auto& family) {
        return family->getTag() == tag;
      });
    }
  }
  if (auto endSyncIt = surfaceContext.endSyncValues.find(tag);
      endSyncIt != surfaceContext.endSyncValues.end()) {
    for (const auto& key : keys) {
      endSyncIt->second.values.erase(key);
    }
    if (endSyncIt->second.values.empty()) {
      surfaceContext.endSyncValues.erase(endSyncIt);
    }
  }
}

bool AnimatedPropsRegistry::isConnected(uint64_t connection) const {
  auto connectionIt = connections_.find(connection);
  if (connectionIt == connections_.end()) {
    return false;
  }
  auto nodeIt = nodeConnections_.find(connectionIt->second.propsNodeTag);
  return nodeIt != nodeConnections_.end() && nodeIt->second == connection;
}

std::optional<uint64_t> AnimatedPropsRegistry::disconnect(Tag propsNodeTag) {
  auto lock = std::lock_guard(mutex_);
  auto it = nodeConnections_.find(propsNodeTag);
  if (it == nodeConnections_.end()) {
    return std::nullopt;
  }
  auto connection = it->second;
  nodeConnections_.erase(it);
  eraseIfUnreferenced(connection);
  return connection;
}

void AnimatedPropsRegistry::eraseIfUnreferenced(uint64_t connectionId) {
  auto connectionIt = connections_.find(connectionId);
  if (connectionIt == connections_.end() || isConnected(connectionId)) {
    return;
  }
  const auto& connection = connectionIt->second;
  auto family = connection.family.lock();
  auto viewsIt = directWrites_.find(connection.surfaceId);
  if (family != nullptr && viewsIt != directWrites_.end()) {
    if (auto viewIt = viewsIt->second.find(family->getTag());
        viewIt != viewsIt->second.end()) {
      const auto& view = viewIt->second;
      auto isOwner = [&](const auto& entry) {
        return entry.second == connectionId;
      };
      auto isWriter = [&](const auto& entry) {
        return entry.second.connection == connectionId;
      };
      if (std::any_of(view.ownedKeys.begin(), view.ownedKeys.end(), isOwner) ||
          std::any_of(view.rawProps.begin(), view.rawProps.end(), isWriter)) {
        return;
      }
    }
  }
  connections_.erase(connectionIt);
}

std::vector<SurfaceId> AnimatedPropsRegistry::surfacesOf(
    const std::vector<uint64_t>& connections) const {
  auto lock = std::lock_guard(mutex_);
  std::vector<SurfaceId> surfaceIds;
  for (auto connection : connections) {
    if (auto it = connections_.find(connection); it != connections_.end() &&
        std::find(surfaceIds.begin(), surfaceIds.end(), it->second.surfaceId) ==
            surfaceIds.end()) {
      surfaceIds.push_back(it->second.surfaceId);
    }
  }
  return surfaceIds;
}

std::pair<uint64_t, std::vector<std::shared_ptr<const ShadowNodeFamily>>>
AnimatedPropsRegistry::releaseOnSurface(
    SurfaceId surfaceId,
    const std::vector<uint64_t>& connections) {
  auto lock = std::lock_guard(mutex_);
  auto releaseNumber = ++releaseNumber_;
  std::vector<std::shared_ptr<const ShadowNodeFamily>> families;
  auto contextIt = surfaceContexts_.find(surfaceId);
  auto viewsIt = directWrites_.find(surfaceId);
  if (contextIt == surfaceContexts_.end() || viewsIt == directWrites_.end()) {
    return {releaseNumber, families};
  }
  auto& surfaceContext = contextIt->second;
  for (auto connectionId : connections) {
    auto connectionIt = connections_.find(connectionId);
    if (connectionIt == connections_.end() ||
        connectionIt->second.surfaceId != surfaceId) {
      continue;
    }
    auto& connection = connectionIt->second;
    auto family = connection.family.lock();
    if (family == nullptr) {
      continue;
    }
    auto viewIt = viewsIt->second.find(family->getTag());
    if (viewIt == viewsIt->second.end()) {
      continue;
    }
    auto& view = viewIt->second;
    std::vector<std::string> releasedKeys;
    std::erase_if(view.ownedKeys, [&](const auto& entry) {
      if (entry.second != connectionId) {
        return false;
      }
      releasedKeys.push_back(entry.first);
      return true;
    });
    bool isWritten = std::any_of(
        view.rawProps.begin(), view.rawProps.end(), [&](const auto& entry) {
          return entry.second.connection == connectionId;
        });
    if (releasedKeys.empty() && !isWritten) {
      eraseIfUnreferenced(connectionId);
      continue;
    }
    connection.releasedAt = releaseNumber;
    eraseOwnedKeys(surfaceContext, family->getTag(), releasedKeys);
    if (isWritten &&
        std::find(families.begin(), families.end(), family) == families.end()) {
      surfaceContext.releasedFamilies.emplace_back(releaseNumber, family);
      families.push_back(std::move(family));
    }
    eraseIfUnreferenced(connectionId);
  }
  return {releaseNumber, families};
}

void AnimatedPropsRegistry::initializeSurface(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  surfaceContexts_.try_emplace(surfaceId);
}

StampingSnapshot AnimatedPropsRegistry::takeStampingSnapshot(
    SurfaceId surfaceId,
    bool endSync) {
  auto lock = std::lock_guard(mutex_);
  auto contextIt = surfaceContexts_.find(surfaceId);
  if (contextIt == surfaceContexts_.end()) {
    return {.number = writeNumber_};
  }
  auto& surfaceContext = contextIt->second;
  auto& pendingMap = surfaceContext.pendingMap;
  auto& map = surfaceContext.map;
  auto& pendingFamilies = surfaceContext.pendingFamilies;
  auto& families = surfaceContext.families;

  for (auto& family : pendingFamilies) {
    families.insert(family);
  }
  for (auto& [tag, propsSnapshot] : pendingMap) {
    auto currentIt = map.find(tag);
    if (currentIt == map.end()) {
      map.insert_or_assign(tag, std::move(propsSnapshot));
    } else {
      auto& currentSnapshot = currentIt->second;
      for (auto propName : propsSnapshot->propNames) {
        auto edgesIt = propsSnapshot->layoutEdges.find(propName);
        supersedeRawProps(
            *currentSnapshot,
            propName,
            edgesIt != propsSnapshot->layoutEdges.end()
                ? edgesIt->second
                : std::unordered_set<yoga::Edge>{});
      }
      if (propsSnapshot->rawProps) {
        supersedeTypedProps(*currentSnapshot, *propsSnapshot->rawProps);
        mergeRawProps(currentSnapshot->rawProps, *propsSnapshot->rawProps);
      }
      if (!propsSnapshot->propNames.empty() && !currentSnapshot->props) {
        currentSnapshot->props = std::make_unique<BaseViewProps>();
      }
      for (auto propName : propsSnapshot->propNames) {
        copyTypedProp(propName, *currentSnapshot, *propsSnapshot);
      }
    }
  }
  pendingMap.clear();
  pendingFamilies.clear();
  if (auto viewsIt = directWrites_.find(surfaceId);
      viewsIt != directWrites_.end()) {
    std::erase_if(viewsIt->second, [](const auto& entry) {
      return entry.second.family.expired();
    });
    if (viewsIt->second.empty()) {
      directWrites_.erase(viewsIt);
    }
  }
  std::erase_if(surfaceContext.endSyncValues, [](const auto& entry) {
    return entry.second.family.expired();
  });
  std::erase_if(surfaceContext.releasedFamilies, [](const auto& entry) {
    return entry.second.expired();
  });
  std::erase_if(connections_, [&](const auto& entry) {
    const auto& [connectionId, connection] = entry;
    if (connection.surfaceId != surfaceId || !connection.family.expired()) {
      return false;
    }
    if (auto nodeIt = nodeConnections_.find(connection.propsNodeTag);
        nodeIt != nodeConnections_.end() && nodeIt->second == connectionId) {
      nodeConnections_.erase(nodeIt);
    }
    return true;
  });

  StampingSnapshot snapshot{.number = writeNumber_, .families = families};
  for (const auto& [tag, propsSnapshot] : map) {
    snapshot.props[tag] = PropsSnapshot{
        .props = propsSnapshot->props
            ? std::make_unique<BaseViewProps>(*propsSnapshot->props)
            : nullptr,
        .propNames = propsSnapshot->propNames,
        .layoutEdges = propsSnapshot->layoutEdges,
        .rawProps = propsSnapshot->rawProps
            ? std::make_unique<folly::dynamic>(*propsSnapshot->rawProps)
            : nullptr};
  }
  if (!endSync) {
    return snapshot;
  }
  surfaceContext.endSyncNumber = writeNumber_;
  for (const auto& [tag, endSyncValues] : surfaceContext.endSyncValues) {
    auto owned = ownedKeys(surfaceId, tag);
    auto family = endSyncValues.family.lock();
    for (const auto& [key, write] : endSyncValues.values) {
      if (auto ownerIt = owned.find(key); family == nullptr ||
          ownerIt == owned.end() || ownerIt->second != write.connection) {
        continue;
      }
      auto& props = snapshot.props[tag];
      if (!props.rawProps) {
        props.rawProps =
            std::make_unique<folly::dynamic>(folly::dynamic::object());
      }
      (*props.rawProps)[key] = write.value;
      snapshot.families.insert(family);
    }
  }
  return snapshot;
}

void AnimatedPropsRegistry::clear(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  if (auto it = surfaceContexts_.find(surfaceId);
      it != surfaceContexts_.end()) {
    auto& surfaceContext = it->second;
    surfaceContext.families.clear();
    surfaceContext.map.clear();
    std::erase_if(surfaceContext.endSyncValues, [&](auto& entry) {
      std::erase_if(entry.second.values, [&](const auto& value) {
        return value.second.number <= surfaceContext.endSyncNumber;
      });
      return entry.second.values.empty();
    });
  }
}

void AnimatedPropsRegistry::clearOnSurfaceStop(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  surfaceContexts_.erase(surfaceId);
  directWrites_.erase(surfaceId);
  std::erase_if(connections_, [&](const auto& entry) {
    return entry.second.surfaceId == surfaceId;
  });
  std::erase_if(nodeConnections_, [&](const auto& entry) {
    return !connections_.contains(entry.second);
  });
}

bool AnimatedPropsRegistry::wins(
    const RecordedWrite& write,
    const StampedView& group) const {
  if (write.connection == 0) {
    return write.number > group.stamp;
  }
  auto connectionIt = connections_.find(write.connection);
  return connectionIt != connections_.end() &&
      (connectionIt->second.releasedAt == 0 ||
       group.releaseCoverage < connectionIt->second.releasedAt);
}

folly::dynamic AnimatedPropsRegistry::newerDirectWrites(
    const DirectWrites& view,
    const std::optional<StampedView>& group) const {
  folly::dynamic values = folly::dynamic::object();
  std::unordered_map<std::string, uint64_t> numbers;
  auto addValue = [&](const std::string& key, const RecordedWrite& write) {
    if ((!group || wins(write, *group)) && write.number > numbers[key]) {
      values[key] = write.value;
      numbers[key] = write.number;
    }
  };
  for (const auto& [propName, number] : view.typedPropNumbers) {
    if (!group || number > group->stamp) {
      auto packed = animationbackend::packViewProp(propName, *view.typedProps);
      for (const auto& [key, value] : packed.items()) {
        addValue(
            key.getString(),
            {.number = number, .value = value, .connection = 0});
      }
    }
  }
  for (const auto& [key, write] : view.rawProps) {
    addValue(key, write);
  }
  return values;
}

std::vector<folly::dynamic> AnimatedPropsRegistry::newerDirectWrites(
    SurfaceId surfaceId,
    const std::vector<StampedView>& stampedViews) const {
  auto lock = std::lock_guard(mutex_);
  auto viewsIt = directWrites_.find(surfaceId);
  if (viewsIt == directWrites_.end()) {
    return {};
  }
  std::vector<folly::dynamic> values;
  values.reserve(stampedViews.size());
  for (const auto& stampedView : stampedViews) {
    auto viewIt = viewsIt->second.find(stampedView.tag);
    values.push_back(
        viewIt != viewsIt->second.end()
            ? newerDirectWrites(viewIt->second, stampedView)
            : folly::dynamic::object());
  }
  return values;
}

#ifdef RN_SERIALIZABLE_STATE
folly::dynamic restoreValues(
    SurfaceId surfaceId,
    const ComponentDescriptor& componentDescriptor,
    const Props::Shared& newProps,
    const folly::dynamic& assumedNative) {
  PropsParserContext propsParserContext{
      surfaceId, *componentDescriptor.getContextContainer()};
  auto assumedProps = componentDescriptor.cloneProps(
      propsParserContext, newProps, RawProps(assumedNative));
  auto diff = newProps->getDiffProps(assumedProps.get());
  folly::dynamic values = folly::dynamic::object();
  for (const auto& [key, value] : diff.items()) {
    if (assumedNative.count(key) != 0) {
      values[key] = value;
    }
  }
  return values;
}

folly::dynamic AnimatedPropsRegistry::restoreDirectWrites(
    SurfaceId surfaceId,
    Tag tag,
    const Props::Shared& props,
    const Props::Shared& preallocatedProps) const {
  std::shared_ptr<const ShadowNodeFamily> family;
  folly::dynamic writtenValues;
  {
    auto lock = std::lock_guard(mutex_);
    auto viewsIt = directWrites_.find(surfaceId);
    if (viewsIt == directWrites_.end()) {
      return folly::dynamic::object();
    }
    auto viewIt = viewsIt->second.find(tag);
    if (viewIt == viewsIt->second.end()) {
      return folly::dynamic::object();
    }
    family = viewIt->second.family.lock();
    if (family == nullptr) {
      return folly::dynamic::object();
    }
    writtenValues = newerDirectWrites(viewIt->second, std::nullopt);
  }
  auto values = restoreValues(
      surfaceId, family->getComponentDescriptor(), props, writtenValues);
  // Without preallocated props, the view was created with default values for
  // the props its creation did not send.
  auto initializationDiff = props->getDiffProps(preallocatedProps.get());
  for (const auto& [key, value] : initializationDiff.items()) {
    if (writtenValues.count(key) != 0) {
      values[key] = value;
    }
  }
  return values;
}

folly::dynamic AnimatedPropsRegistry::releasedValues(
    SurfaceId surfaceId,
    Tag tag,
    uint64_t afterReleaseCoverage,
    uint64_t releaseCoverage,
    const Props::Shared& props) const {
  std::shared_ptr<const ShadowNodeFamily> family;
  folly::dynamic ownedValues = folly::dynamic::object();
  {
    auto lock = std::lock_guard(mutex_);
    auto viewsIt = directWrites_.find(surfaceId);
    if (viewsIt == directWrites_.end()) {
      return folly::dynamic::object();
    }
    auto viewIt = viewsIt->second.find(tag);
    if (viewIt == viewsIt->second.end()) {
      return folly::dynamic::object();
    }
    for (const auto& [key, write] : viewIt->second.rawProps) {
      auto connectionIt = connections_.find(write.connection);
      if (connectionIt != connections_.end() &&
          connectionIt->second.releasedAt > afterReleaseCoverage &&
          connectionIt->second.releasedAt <= releaseCoverage) {
        ownedValues[key] = write.value;
      }
    }
    family = viewIt->second.family.lock();
  }
  if (ownedValues.empty() || family == nullptr) {
    return folly::dynamic::object();
  }
  return restoreValues(
      surfaceId, family->getComponentDescriptor(), props, ownedValues);
}
#endif

bool AnimatedPropsRegistry::hasDirectWrites(SurfaceId surfaceId) const {
  auto lock = std::lock_guard(mutex_);
  return directWrites_.contains(surfaceId);
}

MountingTransaction AnimatedPropsRegistry::finalizeTransaction(
    MountingTransaction transaction,
    const ShadowNode& root) const {
  auto surfaceId = transaction.getSurfaceId();
  auto telemetry = transaction.getTelemetry();
  StampedView group{
      .tag = 0,
      .stamp = telemetry.getAnimationStampNumber(),
      .releaseCoverage = telemetry.getReleaseCoverage()};
  std::vector<std::shared_ptr<const ShadowNodeFamily>> releasedFamilies;
  bool hasDirectWrites = false;
  {
    auto lock = std::lock_guard(mutex_);
    if (auto contextIt = surfaceContexts_.find(surfaceId);
        contextIt != surfaceContexts_.end() &&
        group.releaseCoverage > contextIt->second.pulledReleaseCoverage) {
      auto& surfaceContext = contextIt->second;
      auto& released = surfaceContext.releasedFamilies;
      auto coveredEnd = std::find_if(
          released.begin(), released.end(), [&](const auto& entry) {
            return entry.first > group.releaseCoverage;
          });
      for (auto it = released.begin(); it != coveredEnd; it++) {
        if (auto family = it->second.lock(); family != nullptr &&
            std::find(
                releasedFamilies.begin(), releasedFamilies.end(), family) ==
                releasedFamilies.end()) {
          releasedFamilies.push_back(std::move(family));
        }
      }
      released.erase(released.begin(), coveredEnd);
      surfaceContext.pulledReleaseCoverage = group.releaseCoverage;
    }
#ifndef ANDROID
    // Android judges direct writes when it executes mount batches.
    hasDirectWrites = directWrites_.contains(surfaceId);
#endif
  }
  if (releasedFamilies.empty() && !hasDirectWrites) {
    return transaction;
  }
  auto number = transaction.getNumber();
  auto mutations = std::move(transaction).getMutations();
  if (!releasedFamilies.empty()) {
    updateReleasedViews(surfaceId, mutations, root, releasedFamilies);
  }
  if (hasDirectWrites) {
    overlayNewerDirectWrites(surfaceId, mutations, group);
  }
  return MountingTransaction{
      surfaceId, number, std::move(mutations), telemetry};
}

void AnimatedPropsRegistry::overlayNewerDirectWrites(
    SurfaceId surfaceId,
    ShadowViewMutationList& mutations,
    const StampedView& group) const {
  struct Overlay {
    size_t mutationIndex;
    std::shared_ptr<const ShadowNodeFamily> family;
    folly::dynamic values;
  };
  std::vector<Overlay> overlays;
  {
    auto lock = std::lock_guard(mutex_);
    auto viewsIt = directWrites_.find(surfaceId);
    if (viewsIt == directWrites_.end()) {
      return;
    }
    for (size_t i = 0; i < mutations.size(); i++) {
      const auto& mutation = mutations[i];
      if (mutation.type == ShadowViewMutation::Remove ||
          mutation.type == ShadowViewMutation::Delete) {
        continue;
      }
      auto viewIt = viewsIt->second.find(mutation.newChildShadowView.tag);
      if (viewIt == viewsIt->second.end()) {
        continue;
      }
      auto family = viewIt->second.family.lock();
      auto values = newerDirectWrites(viewIt->second, group);
      if (family && !values.empty()) {
        overlays.push_back({i, std::move(family), std::move(values)});
      }
    }
  }
  for (auto& [mutationIndex, family, values] : overlays) {
    const auto& componentDescriptor = family->getComponentDescriptor();
    PropsParserContext propsParserContext{
        surfaceId, *componentDescriptor.getContextContainer()};
    auto& props = mutations[mutationIndex].newChildShadowView.props;
    props = componentDescriptor.cloneProps(
        propsParserContext, props, RawProps(std::move(values)));
  }
}

} // namespace facebook::react
