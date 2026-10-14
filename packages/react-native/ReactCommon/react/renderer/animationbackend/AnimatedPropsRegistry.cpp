/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnimatedPropsRegistry.h"
#include <react/debug/react_native_assert.h>
#include <react/renderer/core/PropsParserContext.h>
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

void AnimatedPropsRegistry::update(
    const std::vector<AnimationMutations>& batches) {
  auto lock = std::lock_guard(mutex_);
  for (const auto& mutations : batches) {
    for (const auto& mutation : mutations.batch) {
      recordCommitted(0, mutation);
    }
  }
}

void AnimatedPropsRegistry::update(const FrontendMutations& batch) {
  auto lock = std::lock_guard(mutex_);
  for (const auto& mutation : batch.mutations.batch) {
    if (batch.commitDirectProps || mutation.hasLayoutUpdates) {
      recordCommitted(batch.frontendId, mutation);
    } else {
      recordUncommitted(batch.frontendId, mutation);
    }
  }
}

// A raw value of the key supersedes the typed value written as it. A typed
// border prop replaces its whole group, so its other values become raw values.
static void eraseTypedValue(PropsSnapshot& snapshot, const std::string& key) {
  if (auto propName = animationbackend::singleKeyPropName(key)) {
    snapshot.propNames.erase(*propName);
    return;
  }
  auto propName = animationbackend::borderPropName(key);
  if (!propName || !snapshot.propNames.erase(*propName)) {
    return;
  }
  if (!snapshot.rawProps) {
    snapshot.rawProps =
        std::make_unique<folly::dynamic>(folly::dynamic::object());
  }
  auto values = animationbackend::packBorderProp(*propName, *snapshot.props);
  for (const auto& [valueKey, value] : values.items()) {
    if (valueKey.getString() != key) {
      (*snapshot.rawProps)[valueKey] = value;
    }
  }
}

static void eraseKey(PropsSnapshot& snapshot, const std::string& key) {
  eraseTypedValue(snapshot, key);
  if (snapshot.rawProps) {
    snapshot.rawProps->erase(key);
  }
}

// The raw keys whose values the typed prop writes, its whole group for a
// border prop.
static std::vector<std::string> rawKeysOf(
    const PropsSnapshot& snapshot,
    PropName propName) {
  std::vector<std::string> keys;
  if (snapshot.rawProps) {
    for (const auto& [key, _] : snapshot.rawProps->items()) {
      auto keyName = key.getString();
      if (animationbackend::propNameOfKey(keyName) == propName) {
        keys.push_back(std::move(keyName));
      }
    }
  }
  return keys;
}

// A typed prop supersedes the earlier raw values it writes.
static void eraseRawValues(PropsSnapshot& snapshot, PropName propName) {
  for (const auto& key : rawKeysOf(snapshot, propName)) {
    snapshot.rawProps->erase(key);
  }
}

// Whether the snapshot stamps a value of the key.
static bool stampsKey(const PropsSnapshot& snapshot, const std::string& key) {
  if (snapshot.rawProps && snapshot.rawProps->count(key) != 0) {
    return true;
  }
  auto propName = animationbackend::singleKeyPropName(key);
  if (!propName) {
    propName = animationbackend::borderPropName(key);
  }
  return propName && snapshot.propNames.contains(*propName);
}

static bool isEmpty(const PropsSnapshot& snapshot) {
  return snapshot.propNames.empty() &&
      (!snapshot.rawProps || snapshot.rawProps->empty());
}

static StampedProp stampedProp(const std::string& key) {
  if (auto propName = animationbackend::propNameOfKey(key)) {
    return *propName;
  }
  return key;
}

// Whether the snapshot stamps a value of the prop.
static bool stampsProp(const PropsSnapshot& snapshot, const StampedProp& prop) {
  if (const auto* propName = std::get_if<PropName>(&prop)) {
    return snapshot.propNames.contains(*propName) ||
        !rawKeysOf(snapshot, *propName).empty();
  }
  return snapshot.rawProps &&
      snapshot.rawProps->count(std::get<std::string>(prop)) != 0;
}

static void eraseProp(PropsSnapshot& snapshot, const StampedProp& prop) {
  if (const auto* propName = std::get_if<PropName>(&prop)) {
    snapshot.propNames.erase(*propName);
    eraseRawValues(snapshot, *propName);
  } else if (snapshot.rawProps) {
    snapshot.rawProps->erase(std::get<std::string>(prop));
  }
}

// Forgets the writers of the props no snapshot of the view stamps anymore.
static void forgetUnstampedWriters(
    SurfaceContext& surfaceContext,
    Tag tag,
    const std::vector<StampedProp>& props) {
  auto viewIt = surfaceContext.stamped.find(tag);
  if (viewIt == surfaceContext.stamped.end()) {
    return;
  }
  auto pendingIt = surfaceContext.pendingMap.find(tag);
  auto mapIt = surfaceContext.map.find(tag);
  for (const auto& prop : props) {
    if ((pendingIt == surfaceContext.pendingMap.end() ||
         !stampsProp(*pendingIt->second, prop)) &&
        (mapIt == surfaceContext.map.end() ||
         !stampsProp(*mapIt->second, prop))) {
      viewIt->second.writers.erase(prop);
    }
  }
  if (viewIt->second.writers.empty()) {
    surfaceContext.stamped.erase(viewIt);
  }
}

static std::shared_ptr<PropsSnapshot> copySnapshot(
    const PropsSnapshot& snapshot) {
  return std::make_shared<PropsSnapshot>(PropsSnapshot{
      .props = snapshot.props,
      .propNames = snapshot.propNames,
      .rawProps = snapshot.rawProps
          ? std::make_unique<folly::dynamic>(*snapshot.rawProps)
          : nullptr});
}

void AnimatedPropsRegistry::recordCommitted(
    AnimationFrontendId frontendId,
    const AnimationMutation& mutation) {
  const auto& family = mutation.family;
  react_native_assert(family != nullptr);
  auto contextIt = surfaceContexts_.find(family->getSurfaceId());
  if (contextIt == surfaceContexts_.end()) {
    return;
  }
  const auto& animatedProps = mutation.props;
  folly::dynamic converted;
  const folly::dynamic* newValues = nullptr;
  if (animatedProps.rawProps) {
    newValues = animatedProps.rawProps->getDynamic();
    if (newValues == nullptr) {
      converted = animatedProps.rawProps->toDynamic();
      newValues = &converted;
    }
  }
  if (animatedProps.props.empty() &&
      (newValues == nullptr || newValues->empty())) {
    return;
  }
  auto& surfaceContext = contextIt->second;
  auto& pendingMap = surfaceContext.pendingMap;
  surfaceContext.pendingFamilies.insert(family);
  const auto tag = mutation.tag;
  auto it = pendingMap.find(tag);
  if (it == pendingMap.end()) {
    it = pendingMap.insert_or_assign(tag, std::make_unique<PropsSnapshot>())
             .first;
  }
  auto& snapshot = it->second;
  auto& stampedView = surfaceContext.stamped[tag];
  stampedView.family = family;
  if (newValues != nullptr) {
    for (const auto& [key, _] : newValues->items()) {
      eraseTypedValue(*snapshot, key.getString());
      stampedView.writers[stampedProp(key.getString())] = {
          .frontendId = frontendId};
    }
    if (snapshot->rawProps) {
      mergeAnimatedRawProps(*snapshot->rawProps, *newValues);
    } else {
      snapshot->rawProps = std::make_unique<folly::dynamic>(*newValues);
    }
  }
  if (!animatedProps.props.empty() && !snapshot->props) {
    snapshot->props = std::make_shared<BaseViewProps>();
  }
  auto& map = surfaceContext.map;
  auto mapIt = map.find(tag);
  for (const auto& animatedProp : animatedProps.props) {
    const auto propName = animatedProp->propName;
    eraseRawValues(*snapshot, propName);
    // The typed value replaces the consumed value of the prop too.
    if (mapIt != map.end() &&
        (mapIt->second->propNames.contains(propName) ||
         !rawKeysOf(*mapIt->second, propName).empty())) {
      auto stamped = copySnapshot(*mapIt->second);
      eraseRawValues(*stamped, propName);
      stamped->propNames.erase(propName);
      if (isEmpty(*stamped)) {
        map.erase(mapIt);
        mapIt = map.end();
        surfaceContext.families.erase(family);
      } else {
        mapIt->second = std::move(stamped);
      }
    }
    snapshot->propNames.insert(propName);
    cloneProp(*snapshot->props, *animatedProp);
    stampedView.writers[propName] = {.frontendId = frontendId};
  }

  // A committed write supersedes an uncommitted value of the same key.
  auto& uncommitted = surfaceContext.uncommitted;
  if (auto viewIt = uncommitted.find(tag); viewIt != uncommitted.end()) {
    std::erase_if(viewIt->second.values, [&](const auto& item) {
      return animationbackend::writesKey(newValues, animatedProps, item.first);
    });
    if (viewIt->second.values.empty()) {
      uncommitted.erase(viewIt);
    }
  }
}

void AnimatedPropsRegistry::recordUncommitted(
    AnimationFrontendId frontendId,
    const AnimationMutation& mutation) {
  const auto& family = mutation.family;
  react_native_assert(family != nullptr);
  auto contextIt = surfaceContexts_.find(family->getSurfaceId());
  if (contextIt == surfaceContexts_.end()) {
    return;
  }
  auto values = animationbackend::packAnimatedProps(mutation.props);
  if (values.empty()) {
    return;
  }
  auto& surfaceContext = contextIt->second;
  auto& view = surfaceContext.uncommitted[mutation.tag];
  view.family = family;
  auto& pendingMap = surfaceContext.pendingMap;
  auto& map = surfaceContext.map;
  auto pendingIt = pendingMap.find(mutation.tag);
  auto mapIt = map.find(mutation.tag);
  std::shared_ptr<PropsSnapshot> stamped;
  std::vector<StampedProp> takenOver;
  for (const auto& [key, value] : values.items()) {
    auto keyName = key.getString();
    takenOver.push_back(stampedProp(keyName));
    view.values[keyName] = {.value = value, .writer = frontendId};
    if (pendingIt != pendingMap.end()) {
      eraseKey(*pendingIt->second, keyName);
    }
    if (mapIt != map.end() &&
        stampsKey(stamped ? *stamped : *mapIt->second, keyName)) {
      if (!stamped) {
        stamped = copySnapshot(*mapIt->second);
      }
      eraseKey(*stamped, keyName);
    }
  }
  if (pendingIt != pendingMap.end() && isEmpty(*pendingIt->second)) {
    pendingMap.erase(pendingIt);
    surfaceContext.pendingFamilies.erase(family);
  }
  if (stamped && isEmpty(*stamped)) {
    map.erase(mapIt);
    surfaceContext.families.erase(family);
  } else if (stamped) {
    mapIt->second = std::move(stamped);
  }
  forgetUnstampedWriters(surfaceContext, mutation.tag, takenOver);
}

std::set<SurfaceId> AnimatedPropsRegistry::release(
    AnimationFrontendId frontendId,
    const std::vector<AnimatedPropsRelease>& releases,
    std::vector<AnimationMutation>& restoredWrites) {
  auto lock = std::lock_guard(mutex_);
  std::set<SurfaceId> surfaces;
  auto release = ++releaseNumber_;
  for (const auto& [family, keys, commitValues, restoredValues] : releases) {
    auto surfaceId = family->getSurfaceId();
    auto contextIt = surfaceContexts_.find(surfaceId);
    if (contextIt == surfaceContexts_.end()) {
      continue;
    }
    auto& surfaceContext = contextIt->second;
    if (auto stampedIt = surfaceContext.stamped.find(family->getTag());
        stampedIt != surfaceContext.stamped.end()) {
      auto viewIt = surfaceContext.uncommitted.find(family->getTag());
      folly::dynamic owned = folly::dynamic::object();
      for (const auto& key : keys) {
        auto writerIt = stampedIt->second.writers.find(stampedProp(key));
        // A group's stamped writer does not own a key taken over uncommitted.
        if (restoredValues.count(key) != 0 &&
            writerIt != stampedIt->second.writers.end() &&
            writerIt->second.frontendId == frontendId &&
            (viewIt == surfaceContext.uncommitted.end() ||
             !viewIt->second.values.contains(key))) {
          owned[key] = restoredValues[key];
        }
      }
      if (!owned.empty()) {
        AnimationMutation restoredWrite{
            .tag = family->getTag(),
            .family = family,
            .props = {
                .props = {},
                .rawProps = std::make_unique<RawProps>(std::move(owned))}};
        recordCommitted(frontendId, restoredWrite);
        restoredWrites.push_back(std::move(restoredWrite));
      }
    }
    auto viewIt = surfaceContext.uncommitted.find(family->getTag());
    auto stampedIt = surfaceContext.stamped.find(family->getTag());
    for (const auto& key : keys) {
      if (viewIt != surfaceContext.uncommitted.end()) {
        auto valueIt = viewIt->second.values.find(key);
        if (valueIt != viewIt->second.values.end() &&
            valueIt->second.writer == frontendId) {
          valueIt->second.release = release;
          valueIt->second.commitValue = commitValues;
          valueIt->second.isReleaseCommitted = false;
          surfaces.insert(surfaceId);
        }
      }
      // React adopted stamped values already, so they are adopted either way.
      if (stampedIt != surfaceContext.stamped.end()) {
        auto writerIt = stampedIt->second.writers.find(stampedProp(key));
        if (writerIt != stampedIt->second.writers.end() &&
            writerIt->second.frontendId == frontendId) {
          writerIt->second.release = release;
          surfaces.insert(surfaceId);
        }
      }
    }
  }
  return surfaces;
}

ReleaseCommit AnimatedPropsRegistry::releaseCommit(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  ReleaseCommit commit{.release = releaseNumber_};
  auto contextIt = surfaceContexts_.find(surfaceId);
  if (contextIt == surfaceContexts_.end()) {
    return commit;
  }
  auto& uncommitted = contextIt->second.uncommitted;
  std::unordered_set<Tag> selectedTags;
  for (auto viewIt = uncommitted.begin(); viewIt != uncommitted.end();) {
    const auto& [tag, view] = *viewIt;
    auto family = view.family.lock();
    if (!family) {
      viewIt = uncommitted.erase(viewIt);
      continue;
    }
    ++viewIt;
    folly::dynamic adoptedValues = folly::dynamic::object();
    bool isReleased = false;
    for (const auto& [key, value] : view.values) {
      if (value.release == 0 || value.isReleaseCommitted) {
        continue;
      }
      isReleased = true;
      if (value.commitValue) {
        adoptedValues[key] = value.value;
      }
    }
    if (!isReleased) {
      continue;
    }
    selectedTags.insert(tag);
    commit.families.push_back(std::move(family));
    if (!adoptedValues.empty()) {
      commit.adoptedValues.emplace(tag, std::move(adoptedValues));
    }
  }
  for (const auto& [tag, view] : contextIt->second.stamped) {
    auto family = view.family.lock();
    if (family && !selectedTags.contains(tag) &&
        std::any_of(
            view.writers.begin(), view.writers.end(), [](const auto& writer) {
              return writer.second.release != 0;
            })) {
      commit.families.push_back(std::move(family));
    }
  }
  return commit;
}

void AnimatedPropsRegistry::didCommitReleases(
    SurfaceId surfaceId,
    uint64_t release) {
  auto lock = std::lock_guard(mutex_);
  auto contextIt = surfaceContexts_.find(surfaceId);
  if (contextIt == surfaceContexts_.end()) {
    return;
  }
  for (auto& [tag, view] : contextIt->second.uncommitted) {
    for (auto& [key, value] : view.values) {
      if (value.release != 0 && value.release <= release) {
        value.isReleaseCommitted = true;
      }
    }
  }
}

void AnimatedPropsRegistry::stopStamping(
    SurfaceId surfaceId,
    uint64_t release) {
  auto lock = std::lock_guard(mutex_);
  auto contextIt = surfaceContexts_.find(surfaceId);
  if (contextIt == surfaceContexts_.end()) {
    return;
  }
  auto& surfaceContext = contextIt->second;
  for (auto viewIt = surfaceContext.stamped.begin();
       viewIt != surfaceContext.stamped.end();) {
    auto& [tag, view] = *viewIt;
    std::vector<StampedProp> adopted;
    std::erase_if(view.writers, [&](const auto& writer) {
      if (writer.second.release == 0 || writer.second.release > release) {
        return false;
      }
      adopted.push_back(writer.first);
      return true;
    });
    if (adopted.empty()) {
      ++viewIt;
      continue;
    }
    auto family = view.family.lock();
    if (auto it = surfaceContext.pendingMap.find(tag);
        it != surfaceContext.pendingMap.end()) {
      for (const auto& prop : adopted) {
        eraseProp(*it->second, prop);
      }
      if (isEmpty(*it->second)) {
        surfaceContext.pendingMap.erase(it);
        surfaceContext.pendingFamilies.erase(family);
      }
    }
    auto mapIt = surfaceContext.map.find(tag);
    if (mapIt != surfaceContext.map.end() &&
        std::any_of(adopted.begin(), adopted.end(), [&](const auto& prop) {
          return stampsProp(*mapIt->second, prop);
        })) {
      auto stamped = copySnapshot(*mapIt->second);
      for (const auto& prop : adopted) {
        eraseProp(*stamped, prop);
      }
      if (isEmpty(*stamped)) {
        surfaceContext.map.erase(mapIt);
        surfaceContext.families.erase(family);
      } else {
        mapIt->second = std::move(stamped);
      }
    }
    viewIt = view.writers.empty() ? surfaceContext.stamped.erase(viewIt)
                                  : std::next(viewIt);
  }
}

PulledValues AnimatedPropsRegistry::pull(
    SurfaceId surfaceId,
    uint64_t release,
    const std::vector<Tag>& tags) {
  auto lock = std::lock_guard(mutex_);
  PulledValues pulled;
  auto contextIt = surfaceContexts_.find(surfaceId);
  if (contextIt == surfaceContexts_.end()) {
    return pulled;
  }
  std::unordered_set<Tag> pulledTags(tags.begin(), tags.end());
  auto& uncommitted = contextIt->second.uncommitted;
  for (auto viewIt = uncommitted.begin(); viewIt != uncommitted.end();) {
    auto& [tag, view] = *viewIt;
    if (view.family.expired()) {
      viewIt = uncommitted.erase(viewIt);
      continue;
    }
    bool isPulled = pulledTags.contains(tag);
    folly::dynamic overlaid = folly::dynamic::object();
    folly::dynamic restored = folly::dynamic::object();
    for (auto valueIt = view.values.begin(); valueIt != view.values.end();) {
      const auto& [key, value] = *valueIt;
      if (value.release == 0 || value.release > release) {
        if (isPulled) {
          overlaid[key] = value.value;
        }
        ++valueIt;
      } else if (value.commitValue) {
        valueIt = view.values.erase(valueIt);
      } else if (isPulled) {
        restored[key] = value.value;
        valueIt = view.values.erase(valueIt);
      } else {
        ++valueIt;
      }
    }
    if (!overlaid.empty()) {
      pulled.overlaid.emplace(tag, std::move(overlaid));
    }
    if (!restored.empty()) {
      pulled.restored.emplace(tag, std::move(restored));
    }
    viewIt =
        view.values.empty() ? uncommitted.erase(viewIt) : std::next(viewIt);
  }
  return pulled;
}

void AnimatedPropsRegistry::initializeSurface(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  surfaceContexts_.try_emplace(surfaceId);
}

std::pair<
    std::unordered_set<std::shared_ptr<const ShadowNodeFamily>>,
    SnapshotMap>
AnimatedPropsRegistry::getMap(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  auto& surfaceContext = surfaceContexts_[surfaceId];
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
      continue;
    }
    auto snapshot = copySnapshot(*currentIt->second);
    if (propsSnapshot->rawProps) {
      for (const auto& [key, _] : propsSnapshot->rawProps->items()) {
        eraseTypedValue(*snapshot, key.getString());
      }
      if (snapshot->rawProps) {
        mergeAnimatedRawProps(*snapshot->rawProps, *propsSnapshot->rawProps);
      } else {
        snapshot->rawProps = std::move(propsSnapshot->rawProps);
      }
    }
    for (auto propName : propsSnapshot->propNames) {
      eraseRawValues(*snapshot, propName);
    }
    if (!snapshot->props) {
      snapshot->props = std::move(propsSnapshot->props);
      snapshot->propNames = std::move(propsSnapshot->propNames);
    } else if (!propsSnapshot->propNames.empty()) {
      snapshot->props = std::make_shared<BaseViewProps>(*snapshot->props);
      for (auto& propName : propsSnapshot->propNames) {
        snapshot->propNames.insert(propName);
        updateProp(propName, *snapshot->props, *propsSnapshot);
      }
    }
    currentIt->second = std::move(snapshot);
  }
  pendingMap.clear();
  pendingFamilies.clear();

  return {families, map};
}

// React adopted every stamped value in the commit before this. Registered
// frontends keep the props they animate; a released prop ends here too, also
// when the end sync could not stop stamping it.
void AnimatedPropsRegistry::clear(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  auto contextIt = surfaceContexts_.find(surfaceId);
  if (contextIt == surfaceContexts_.end()) {
    return;
  }
  auto& surfaceContext = contextIt->second;
  for (auto mapIt = surfaceContext.map.begin();
       mapIt != surfaceContext.map.end();) {
    const auto tag = mapIt->first;
    auto& snapshot = mapIt->second;
    auto viewIt = surfaceContext.stamped.find(tag);
    auto isCleared = [&](const StampedProp& prop) {
      if (viewIt == surfaceContext.stamped.end()) {
        return true;
      }
      auto writerIt = viewIt->second.writers.find(prop);
      return writerIt == viewIt->second.writers.end() ||
          writerIt->second.frontendId == 0 || writerIt->second.release != 0;
    };
    std::unordered_set<StampedProp> cleared;
    for (auto propName : snapshot->propNames) {
      if (isCleared(propName)) {
        cleared.insert(propName);
      }
    }
    if (snapshot->rawProps) {
      for (const auto& [key, _] : snapshot->rawProps->items()) {
        auto prop = stampedProp(key.getString());
        if (isCleared(prop)) {
          cleared.insert(std::move(prop));
        }
      }
    }
    if (cleared.empty()) {
      ++mapIt;
      continue;
    }
    auto family = viewIt != surfaceContext.stamped.end()
        ? viewIt->second.family.lock()
        : nullptr;
    auto cleaned = copySnapshot(*snapshot);
    for (const auto& prop : cleared) {
      eraseProp(*cleaned, prop);
    }
    if (isEmpty(*cleaned)) {
      surfaceContext.families.erase(family);
      mapIt = surfaceContext.map.erase(mapIt);
    } else {
      snapshot = std::move(cleaned);
      ++mapIt;
    }
    forgetUnstampedWriters(
        surfaceContext, tag, {cleared.begin(), cleared.end()});
  }
}

void AnimatedPropsRegistry::clearOnSurfaceStop(SurfaceId surfaceId) {
  auto lock = std::lock_guard(mutex_);
  surfaceContexts_.erase(surfaceId);
}

} // namespace facebook::react
