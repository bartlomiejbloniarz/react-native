/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Conv.h>
#include <react/renderer/graphics/Transform.h>
#include <react/renderer/graphics/TransformUtils.h>
#include <stdexcept>
#include "AnimatedPropsSerializer.h"

namespace facebook::react {

namespace {

// A length as props accept it: points as a number, percents as a string.
folly::dynamic packValueUnit(const ValueUnit& valueUnit) {
  if (valueUnit.unit == UnitType::Percent) {
    return folly::to<std::string>(valueUnit.value) + "%";
  }
  return valueUnit.value;
}

folly::dynamic packColor(const SharedColor& color) {
  return color ? folly::dynamic(static_cast<int32_t>(*color))
               : folly::dynamic(nullptr);
}

// Packs each set edge as border<Edge><suffix>, as the props parser reads it.
template <typename T, typename PackValue>
void packEdges(
    folly::dynamic& dyn,
    const std::string& suffix,
    const CascadedRectangleEdges<T>& edges,
    PackValue packValue) {
  auto pack = [&](const char* edge, const std::optional<T>& value) {
    if (value.has_value()) {
      dyn[std::string("border") + edge + suffix] = packValue(*value);
    }
  };
  pack("Left", edges.left);
  pack("Top", edges.top);
  pack("Right", edges.right);
  pack("Bottom", edges.bottom);
  pack("Start", edges.start);
  pack("End", edges.end);
  pack("Horizontal", edges.horizontal);
  pack("Vertical", edges.vertical);
  pack("Block", edges.block);
  pack("BlockStart", edges.blockStart);
  pack("BlockEnd", edges.blockEnd);
  pack("", edges.all);
}

// Packs each set corner as border<Corner><suffix>, as the props parser reads
// it.
template <typename T, typename PackValue>
void packCorners(
    folly::dynamic& dyn,
    const std::string& suffix,
    const CascadedRectangleCorners<T>& corners,
    PackValue packValue) {
  auto pack = [&](const char* corner, const std::optional<T>& value) {
    if (value.has_value()) {
      dyn[std::string("border") + corner + suffix] = packValue(*value);
    }
  };
  pack("TopLeft", corners.topLeft);
  pack("TopRight", corners.topRight);
  pack("BottomLeft", corners.bottomLeft);
  pack("BottomRight", corners.bottomRight);
  pack("TopStart", corners.topStart);
  pack("TopEnd", corners.topEnd);
  pack("BottomStart", corners.bottomStart);
  pack("BottomEnd", corners.bottomEnd);
  pack("StartStart", corners.startStart);
  pack("StartEnd", corners.startEnd);
  pack("EndStart", corners.endStart);
  pack("EndEnd", corners.endEnd);
  pack("", corners.all);
}

std::string borderCurveToString(BorderCurve curve) {
  switch (curve) {
    case BorderCurve::Circular:
      return "circular";
    case BorderCurve::Continuous:
      return "continuous";
    default:
      throw std::runtime_error("Unknown border curve");
  }
}

void packBorderRadii(
    folly::dynamic& dyn,
    const CascadedBorderRadii& borderRadii) {
  packCorners(dyn, "Radius", borderRadii, packValueUnit);
}

void packOpacity(folly::dynamic& dyn, const Float& value) {
  dyn.insert("opacity", value);
}

void packTransform(folly::dynamic& dyn, const Transform& transform) {
  auto transformArray = folly::dynamic::array();

  for (const auto& operation : transform.operations) {
    updateTransformProps(transform, operation, transformArray);
  }

  dyn.insert("transform", transformArray);
}

void packTransformOrigin(
    folly::dynamic& dyn,
    const TransformOrigin& transformOrigin) {
  if (!transformOrigin.isSet()) {
    dyn.insert("transformOrigin", nullptr);
    return;
  }
  auto originArray = folly::dynamic::array();
  for (const auto& xyValue : transformOrigin.xy) {
    originArray.push_back(
        xyValue.unit == UnitType::Undefined ? folly::dynamic("50%")
                                            : packValueUnit(xyValue));
  }
  originArray.push_back(transformOrigin.z);
  dyn.insert("transformOrigin", originArray);
}

void packBackgroundColor(
    folly::dynamic& dyn,
    const SharedColor& backgroundColor) {
  dyn.insert("backgroundColor", packColor(backgroundColor));
}

void packShadowColor(folly::dynamic& dyn, const SharedColor& shadowColor) {
  dyn.insert("shadowColor", packColor(shadowColor));
}

void packShadowOffset(folly::dynamic& dyn, const Size& shadowOffset) {
  dyn.insert(
      "shadowOffset",
      folly::dynamic::object("width", shadowOffset.width)(
          "height", shadowOffset.height));
}

void packShadowOpacity(folly::dynamic& dyn, const Float& value) {
  dyn.insert("shadowOpacity", value);
}

void packShadowRadius(folly::dynamic& dyn, const Float& value) {
  dyn.insert("shadowRadius", value);
}

void packBorderColor(
    folly::dynamic& dyn,
    const CascadedBorderColors& borderColors) {
  packEdges(dyn, "Color", borderColors, packColor);
}

void packFilter(
    folly::dynamic& dyn,
    const std::vector<FilterFunction>& filters) {
  auto filterArray = folly::dynamic::array();
  for (const auto& f : filters) {
    folly::dynamic filterObj = folly::dynamic::object();
    std::string typeKey = toString(f.type);
    if (std::holds_alternative<Float>(f.parameters)) {
      filterObj[typeKey] = std::get<Float>(f.parameters);
    } else if (std::holds_alternative<DropShadowParams>(f.parameters)) {
      const auto& dropShadowParams = std::get<DropShadowParams>(f.parameters);
      folly::dynamic shadowObj = folly::dynamic::object();
      shadowObj["offsetX"] = dropShadowParams.offsetX;
      shadowObj["offsetY"] = dropShadowParams.offsetY;
      shadowObj["standardDeviation"] = dropShadowParams.standardDeviation;
      shadowObj["color"] = static_cast<int32_t>(*dropShadowParams.color);
      filterObj[typeKey] = shadowObj;
    }
    filterArray.push_back(filterObj);
  }
  dyn.insert("filter", filterArray);
}

void packOutlineColor(folly::dynamic& dyn, const SharedColor& outlineColor) {
  dyn.insert("outlineColor", packColor(outlineColor));
}

void packOutlineOffset(folly::dynamic& dyn, const Float& value) {
  dyn.insert("outlineOffset", value);
}

void packOutlineStyle(folly::dynamic& dyn, const OutlineStyle& outlineStyle) {
  std::string styleStr;
  switch (outlineStyle) {
    case OutlineStyle::Solid:
      styleStr = "solid";
      break;
    case OutlineStyle::Dotted:
      styleStr = "dotted";
      break;
    case OutlineStyle::Dashed:
      styleStr = "dashed";
      break;
    default:
      throw std::runtime_error("Unknown outline style");
  }
  dyn.insert("outlineStyle", styleStr);
}

void packOutlineWidth(folly::dynamic& dyn, const Float& value) {
  dyn.insert("outlineWidth", value);
}

void packBorderCurves(
    folly::dynamic& dyn,
    const CascadedBorderCurves& borderCurves) {
  packCorners(dyn, "Curve", borderCurves, borderCurveToString);
}

std::string borderStyleToString(BorderStyle style) {
  switch (style) {
    case BorderStyle::Solid:
      return "solid";
    case BorderStyle::Dotted:
      return "dotted";
    case BorderStyle::Dashed:
      return "dashed";
    default:
      throw std::runtime_error("Unknown border style");
  }
}

void packBorderStyles(
    folly::dynamic& dyn,
    const CascadedBorderStyles& borderStyles) {
  packEdges(dyn, "Style", borderStyles, borderStyleToString);
}

void packPointerEvents(
    folly::dynamic& dyn,
    const PointerEventsMode& pointerEvents) {
  std::string pointerEventsStr;
  switch (pointerEvents) {
    case PointerEventsMode::Auto:
      pointerEventsStr = "auto";
      break;
    case PointerEventsMode::None:
      pointerEventsStr = "none";
      break;
    case PointerEventsMode::BoxNone:
      pointerEventsStr = "box-none";
      break;
    case PointerEventsMode::BoxOnly:
      pointerEventsStr = "box-only";
      break;
    default:
      throw std::runtime_error("Unknown pointer events mode");
  }
  dyn.insert("pointerEvents", pointerEventsStr);
}

void packIsolation(folly::dynamic& dyn, const Isolation& isolation) {
  std::string isolationStr;
  switch (isolation) {
    case Isolation::Auto:
      isolationStr = "auto";
      break;
    case Isolation::Isolate:
      isolationStr = "isolate";
      break;
    default:
      throw std::runtime_error("Unknown isolation mode");
  }
  dyn.insert("isolation", isolationStr);
}

void packCursor(folly::dynamic& dyn, const Cursor& cursor) {
  std::string cursorStr;
  switch (cursor) {
    case Cursor::Auto:
      cursorStr = "auto";
      break;
    case Cursor::Alias:
      cursorStr = "alias";
      break;
    case Cursor::AllScroll:
      cursorStr = "all-scroll";
      break;
    case Cursor::Cell:
      cursorStr = "cell";
      break;
    case Cursor::ColResize:
      cursorStr = "col-resize";
      break;
    case Cursor::ContextMenu:
      cursorStr = "context-menu";
      break;
    case Cursor::Copy:
      cursorStr = "copy";
      break;
    case Cursor::Crosshair:
      cursorStr = "crosshair";
      break;
    case Cursor::Default:
      cursorStr = "default";
      break;
    case Cursor::EResize:
      cursorStr = "e-resize";
      break;
    case Cursor::EWResize:
      cursorStr = "ew-resize";
      break;
    case Cursor::Grab:
      cursorStr = "grab";
      break;
    case Cursor::Grabbing:
      cursorStr = "grabbing";
      break;
    case Cursor::Help:
      cursorStr = "help";
      break;
    case Cursor::Move:
      cursorStr = "move";
      break;
    case Cursor::NResize:
      cursorStr = "n-resize";
      break;
    case Cursor::NEResize:
      cursorStr = "ne-resize";
      break;
    case Cursor::NESWResize:
      cursorStr = "nesw-resize";
      break;
    case Cursor::NSResize:
      cursorStr = "ns-resize";
      break;
    case Cursor::NWResize:
      cursorStr = "nw-resize";
      break;
    case Cursor::NWSEResize:
      cursorStr = "nwse-resize";
      break;
    case Cursor::NoDrop:
      cursorStr = "no-drop";
      break;
    case Cursor::None:
      cursorStr = "none";
      break;
    case Cursor::NotAllowed:
      cursorStr = "not-allowed";
      break;
    case Cursor::Pointer:
      cursorStr = "pointer";
      break;
    case Cursor::Progress:
      cursorStr = "progress";
      break;
    case Cursor::RowResize:
      cursorStr = "row-resize";
      break;
    case Cursor::SResize:
      cursorStr = "s-resize";
      break;
    case Cursor::SEResize:
      cursorStr = "se-resize";
      break;
    case Cursor::SWResize:
      cursorStr = "sw-resize";
      break;
    case Cursor::Text:
      cursorStr = "text";
      break;
    case Cursor::Url:
      cursorStr = "url";
      break;
    case Cursor::WResize:
      cursorStr = "w-resize";
      break;
    case Cursor::Wait:
      cursorStr = "wait";
      break;
    case Cursor::ZoomIn:
      cursorStr = "zoom-in";
      break;
    case Cursor::ZoomOut:
      cursorStr = "zoom-out";
      break;
    default:
      throw std::runtime_error("Unknown cursor type");
  }
  dyn.insert("cursor", cursorStr);
}

void packBoxShadow(
    folly::dynamic& dyn,
    const std::vector<BoxShadow>& boxShadows) {
  auto shadowArray = folly::dynamic::array();
  for (const auto& shadow : boxShadows) {
    folly::dynamic shadowObj = folly::dynamic::object();
    shadowObj["offsetX"] = shadow.offsetX;
    shadowObj["offsetY"] = shadow.offsetY;
    shadowObj["blurRadius"] = shadow.blurRadius;
    shadowObj["spreadDistance"] = shadow.spreadDistance;
    shadowObj["inset"] = shadow.inset;
    if (shadow.color) {
      shadowObj["color"] = static_cast<int32_t>(*shadow.color);
    }
    shadowArray.push_back(shadowObj);
  }
  dyn.insert("boxShadow", shadowArray);
}

void packMixBlendMode(folly::dynamic& dyn, const BlendMode& mixBlendMode) {
  std::string blendModeStr;
  switch (mixBlendMode) {
    case BlendMode::Normal:
      blendModeStr = "normal";
      break;
    case BlendMode::Multiply:
      blendModeStr = "multiply";
      break;
    case BlendMode::Screen:
      blendModeStr = "screen";
      break;
    case BlendMode::Overlay:
      blendModeStr = "overlay";
      break;
    case BlendMode::Darken:
      blendModeStr = "darken";
      break;
    case BlendMode::Lighten:
      blendModeStr = "lighten";
      break;
    case BlendMode::ColorDodge:
      blendModeStr = "color-dodge";
      break;
    case BlendMode::ColorBurn:
      blendModeStr = "color-burn";
      break;
    case BlendMode::HardLight:
      blendModeStr = "hard-light";
      break;
    case BlendMode::SoftLight:
      blendModeStr = "soft-light";
      break;
    case BlendMode::Difference:
      blendModeStr = "difference";
      break;
    case BlendMode::Exclusion:
      blendModeStr = "exclusion";
      break;
    case BlendMode::Hue:
      blendModeStr = "hue";
      break;
    case BlendMode::Saturation:
      blendModeStr = "saturation";
      break;
    case BlendMode::Color:
      blendModeStr = "color";
      break;
    case BlendMode::Luminosity:
      blendModeStr = "luminosity";
      break;
    case BlendMode::PlusLighter:
      blendModeStr = "plus-lighter";
      break;
    default:
      throw std::runtime_error("Unknown blend mode");
  }
  dyn.insert("mixBlendMode", blendModeStr);
}

void packBackfaceVisibility(
    folly::dynamic& dyn,
    const BackfaceVisibility& backfaceVisibility) {
  std::string visibilityStr;
  switch (backfaceVisibility) {
    case BackfaceVisibility::Auto:
      visibilityStr = "auto";
      break;
    case BackfaceVisibility::Visible:
      visibilityStr = "visible";
      break;
    case BackfaceVisibility::Hidden:
      visibilityStr = "hidden";
      break;
    default:
      throw std::runtime_error("Unknown backface visibility");
  }
  dyn.insert("backfaceVisibility", visibilityStr);
}

void packAnimatedProp(
    folly::dynamic& dyn,
    const std::unique_ptr<AnimatedPropBase>& animatedProp) {
  switch (animatedProp->propName) {
    case OPACITY:
      packOpacity(dyn, get<Float>(*animatedProp));
      break;

    case TRANSFORM:
      packTransform(dyn, get<Transform>(*animatedProp));
      break;

    case TRANSFORM_ORIGIN:
      packTransformOrigin(dyn, get<TransformOrigin>(*animatedProp));
      break;

    case BACKGROUND_COLOR:
      packBackgroundColor(dyn, get<SharedColor>(*animatedProp));
      break;

    case BORDER_RADII:
      packBorderRadii(dyn, get<CascadedBorderRadii>(*animatedProp));
      break;

    case SHADOW_COLOR:
      packShadowColor(dyn, get<SharedColor>(*animatedProp));
      break;

    case SHADOW_OFFSET:
      packShadowOffset(dyn, get<Size>(*animatedProp));
      break;

    case SHADOW_OPACITY:
      packShadowOpacity(dyn, get<Float>(*animatedProp));
      break;

    case SHADOW_RADIUS:
      packShadowRadius(dyn, get<Float>(*animatedProp));
      break;

    case BORDER_COLOR:
      packBorderColor(dyn, get<CascadedBorderColors>(*animatedProp));
      break;

    case FILTER:
      packFilter(dyn, get<std::vector<FilterFunction>>(*animatedProp));
      break;

    case OUTLINE_COLOR:
      packOutlineColor(dyn, get<SharedColor>(*animatedProp));
      break;

    case OUTLINE_OFFSET:
      packOutlineOffset(dyn, get<Float>(*animatedProp));
      break;

    case OUTLINE_STYLE:
      packOutlineStyle(dyn, get<OutlineStyle>(*animatedProp));
      break;

    case OUTLINE_WIDTH:
      packOutlineWidth(dyn, get<Float>(*animatedProp));
      break;

    case BORDER_CURVES:
      packBorderCurves(dyn, get<CascadedBorderCurves>(*animatedProp));
      break;

    case BORDER_STYLES:
      packBorderStyles(dyn, get<CascadedBorderStyles>(*animatedProp));
      break;

    case POINTER_EVENTS:
      packPointerEvents(dyn, get<PointerEventsMode>(*animatedProp));
      break;

    case ISOLATION:
      packIsolation(dyn, get<Isolation>(*animatedProp));
      break;

    case CURSOR:
      packCursor(dyn, get<Cursor>(*animatedProp));
      break;

    case BOX_SHADOW:
      packBoxShadow(dyn, get<std::vector<BoxShadow>>(*animatedProp));
      break;

    case MIX_BLEND_MODE:
      packMixBlendMode(dyn, get<BlendMode>(*animatedProp));
      break;

    case BACKFACE_VISIBILITY:
      packBackfaceVisibility(dyn, get<BackfaceVisibility>(*animatedProp));
      break;

    case WIDTH:
    case HEIGHT:
    case FLEX:
    case PADDING:
    case MARGIN:
    case POSITION:
    case BORDER_WIDTH:
    case ALIGN_CONTENT:
    case ALIGN_ITEMS:
    case ALIGN_SELF:
    case ASPECT_RATIO:
    case BOX_SIZING:
    case DISPLAY:
    case FLEX_BASIS:
    case FLEX_DIRECTION:
    case ROW_GAP:
    case COLUMN_GAP:
    case FLEX_GROW:
    case FLEX_SHRINK:
    case FLEX_WRAP:
    case JUSTIFY_CONTENT:
    case MAX_HEIGHT:
    case MAX_WIDTH:
    case MIN_HEIGHT:
    case MIN_WIDTH:
    case STYLE_OVERFLOW:
    case POSITION_TYPE:
    case Z_INDEX:
    case DIRECTION:
      break;
    default:
      throw std::runtime_error("Unknown animated prop");
  }
}

} // namespace

namespace animationbackend {

void packAnimatedProp(
    folly::dynamic& dyn,
    const std::unique_ptr<AnimatedPropBase>& animatedProp) {
  react::packAnimatedProp(dyn, animatedProp);
}

bool isWrittenDirectly(PropName propName) {
  switch (propName) {
    case OPACITY:
    case TRANSFORM:
    case TRANSFORM_ORIGIN:
    case BACKGROUND_COLOR:
    case SHADOW_COLOR:
    case SHADOW_OFFSET:
    case SHADOW_OPACITY:
    case SHADOW_RADIUS:
    case FILTER:
    case OUTLINE_COLOR:
    case OUTLINE_OFFSET:
    case OUTLINE_STYLE:
    case OUTLINE_WIDTH:
    case POINTER_EVENTS:
    case ISOLATION:
    case CURSOR:
    case BOX_SHADOW:
    case MIX_BLEND_MODE:
    case BACKFACE_VISIBILITY:
      return true;
    default:
      return false;
  }
}

bool packsPerEdge(PropName propName) {
  return propName == BORDER_RADII || propName == BORDER_COLOR ||
      propName == BORDER_CURVES || propName == BORDER_STYLES;
}

folly::dynamic packViewProp(PropName propName, const BaseViewProps& viewProps) {
  folly::dynamic dyn = folly::dynamic::object();
  switch (propName) {
    case OPACITY:
      packOpacity(dyn, viewProps.opacity);
      break;
    case TRANSFORM:
      packTransform(dyn, viewProps.transform);
      break;
    case TRANSFORM_ORIGIN:
      packTransformOrigin(dyn, viewProps.transformOrigin);
      break;
    case BACKGROUND_COLOR:
      packBackgroundColor(dyn, viewProps.backgroundColor);
      break;
    case SHADOW_COLOR:
      packShadowColor(dyn, viewProps.shadowColor);
      break;
    case SHADOW_OFFSET:
      packShadowOffset(dyn, viewProps.shadowOffset);
      break;
    case SHADOW_OPACITY:
      packShadowOpacity(dyn, viewProps.shadowOpacity);
      break;
    case SHADOW_RADIUS:
      packShadowRadius(dyn, viewProps.shadowRadius);
      break;
    case FILTER:
      packFilter(dyn, viewProps.filter);
      break;
    case OUTLINE_COLOR:
      packOutlineColor(dyn, viewProps.outlineColor);
      break;
    case OUTLINE_OFFSET:
      packOutlineOffset(dyn, viewProps.outlineOffset);
      break;
    case OUTLINE_STYLE:
      packOutlineStyle(dyn, viewProps.outlineStyle);
      break;
    case OUTLINE_WIDTH:
      packOutlineWidth(dyn, viewProps.outlineWidth);
      break;
    case POINTER_EVENTS:
      packPointerEvents(dyn, viewProps.pointerEvents);
      break;
    case ISOLATION:
      packIsolation(dyn, viewProps.isolation);
      break;
    case CURSOR:
      packCursor(dyn, viewProps.cursor);
      break;
    case BOX_SHADOW:
      packBoxShadow(dyn, viewProps.boxShadow);
      break;
    case MIX_BLEND_MODE:
      packMixBlendMode(dyn, viewProps.mixBlendMode);
      break;
    case BACKFACE_VISIBILITY:
      packBackfaceVisibility(dyn, viewProps.backfaceVisibility);
      break;
    default:
      break;
  }
  return dyn;
}

folly::dynamic packAnimatedProps(const AnimatedProps& animatedProps) {
  auto dyn = animatedProps.rawProps ? animatedProps.rawProps->toDynamic()
                                    : folly::dynamic::object();

  for (auto& animatedProp : animatedProps.props) {
    packAnimatedProp(dyn, animatedProp);
  }

  return dyn;
}

} // namespace animationbackend

} // namespace facebook::react
