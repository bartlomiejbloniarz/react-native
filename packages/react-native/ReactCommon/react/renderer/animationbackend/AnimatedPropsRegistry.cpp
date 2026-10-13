/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnimatedPropsRegistry.h"
#include <react/debug/react_native_assert.h>
#include <react/renderer/core/PropsParserContext.h>
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
      recordDirectWrite(mutation, rawProps);

      auto& surfaceContext = contextIt->second;
      surfaceContext.pendingFamilies.insert(family);
      auto& snapshot = surfaceContext.pendingMap[mutation.tag];
      if (!snapshot) {
        snapshot = std::make_unique<PropsSnapshot>();
      }
      if (rawProps != nullptr) {
        supersedeTypedProps(*snapshot, *rawProps);
        mergeRawProps(snapshot->rawProps, *rawProps);
      }
      if (!animatedProps.props.empty() && !snapshot->props) {
        snapshot->props = std::make_unique<BaseViewProps>();
      }
      for (const auto& animatedProp : animatedProps.props) {
        auto propName = animatedProp->propName;
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
    const folly::dynamic* rawProps) {
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
  if (rawProps != nullptr) {
    for (const auto& [key, value] : rawProps->items()) {
      view.rawProps[key.getString()] = {writeNumber_, value};
    }
  }
  for (const auto& animatedProp : mutation.props.props) {
    if (!animationbackend::isWrittenDirectly(animatedProp->propName)) {
      continue;
    }
    if (!view.typedProps) {
      view.typedProps = std::make_unique<BaseViewProps>();
    }
    cloneProp(*view.typedProps, *animatedProp);
    view.typedPropNumbers[animatedProp->propName] = writeNumber_;
  }
}

void AnimatedPropsRegistry::initializeSurface(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  surfaceContexts_.try_emplace(surfaceId);
}

StampingSnapshot AnimatedPropsRegistry::takeStampingSnapshot(
    SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  auto contextIt = surfaceContexts_.find(surfaceId);
  if (contextIt == surfaceContexts_.end()) {
    return {.number = writeNumber_};
  }
  auto& [pendingMap, map, pendingFamilies, families] = contextIt->second;

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
  return snapshot;
}

void AnimatedPropsRegistry::clear(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  if (auto it = surfaceContexts_.find(surfaceId);
      it != surfaceContexts_.end()) {
    auto& surfaceContext = it->second;
    surfaceContext.families.clear();
    surfaceContext.map.clear();
  }
}

void AnimatedPropsRegistry::clearOnSurfaceStop(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  surfaceContexts_.erase(surfaceId);
  directWrites_.erase(surfaceId);
}

folly::dynamic AnimatedPropsRegistry::newerDirectWrites(
    const DirectWrites& view,
    uint64_t stampNumber) const {
  folly::dynamic values = folly::dynamic::object();
  std::unordered_map<std::string, uint64_t> numbers;
  auto addValue = [&](const std::string& key,
                      uint64_t number,
                      const folly::dynamic& value) {
    if (number > stampNumber && number > numbers[key]) {
      values[key] = value;
      numbers[key] = number;
    }
  };
  for (const auto& [propName, number] : view.typedPropNumbers) {
    if (number > stampNumber) {
      auto packed = animationbackend::packViewProp(propName, *view.typedProps);
      for (const auto& [key, value] : packed.items()) {
        addValue(key.getString(), number, value);
      }
    }
  }
  for (const auto& [key, write] : view.rawProps) {
    addValue(key, write.first, write.second);
  }
  return values;
}

std::vector<folly::dynamic> AnimatedPropsRegistry::newerDirectWrites(
    SurfaceId surfaceId,
    const std::vector<std::pair<Tag, uint64_t>>& stampedViews) const {
  auto lock = std::lock_guard(mutex_);
  auto viewsIt = directWrites_.find(surfaceId);
  if (viewsIt == directWrites_.end()) {
    return {};
  }
  std::vector<folly::dynamic> values;
  values.reserve(stampedViews.size());
  for (const auto& [tag, stampNumber] : stampedViews) {
    auto viewIt = viewsIt->second.find(tag);
    values.push_back(
        viewIt != viewsIt->second.end()
            ? newerDirectWrites(viewIt->second, stampNumber)
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
    writtenValues = newerDirectWrites(viewIt->second, 0);
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
#endif

bool AnimatedPropsRegistry::hasDirectWrites(SurfaceId surfaceId) const {
  auto lock = std::lock_guard(mutex_);
  return directWrites_.contains(surfaceId);
}

bool AnimatedPropsRegistry::shouldOverridePullTransaction() const {
#ifdef ANDROID
  // Android judges direct writes when it executes mount batches.
  return false;
#else
  auto lock = std::lock_guard(mutex_);
  return !directWrites_.empty();
#endif
}

std::optional<MountingTransaction> AnimatedPropsRegistry::pullTransaction(
    SurfaceId surfaceId,
    MountingTransaction::Number number,
    const TransactionTelemetry& telemetry,
    ShadowViewMutationList mutations) const {
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
      return MountingTransaction{
          surfaceId, number, std::move(mutations), telemetry};
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
      auto values = newerDirectWrites(
          viewIt->second, telemetry.getAnimationStampNumber());
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
  return MountingTransaction{
      surfaceId, number, std::move(mutations), telemetry};
}

} // namespace facebook::react
