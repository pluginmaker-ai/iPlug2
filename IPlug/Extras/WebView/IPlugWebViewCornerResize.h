/*
 * This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.
 * See LICENSE.txt for more info.
 * PluginMaker alteration: constrain a native corner drag before changing its view.
 */

#pragma once

#include <algorithm>
#include <cmath>

namespace iplug {
namespace webview {

struct CornerSize
{
  int width;
  int height;
};

struct CornerResizeLimits
{
  CornerSize design;
  CornerSize min;
  CornerSize max;
};

inline CornerSize ConstrainCornerWidth(double requestedWidth, CornerResizeLimits limits)
{
  if (limits.design.width <= 0 || limits.design.height <= 0)
    return {std::max(1, limits.min.width), std::max(1, limits.min.height)};

  // Match size-memory bounds: a valid custom design remains a legal size.
  const int minWidth = std::max(1, std::min(limits.min.width, limits.design.width));
  const int minHeight = std::max(1, std::min(limits.min.height, limits.design.height));
  const int maxWidth = std::max(limits.max.width, limits.design.width);
  const int maxHeight = std::max(limits.max.height, limits.design.height);
  const double aspect = static_cast<double>(limits.design.width) / limits.design.height;
  const double widthLo = std::max<double>(minWidth, std::ceil(minHeight * aspect));
  const double widthHi = std::min<double>(maxWidth, std::floor(maxHeight * aspect));

  if (!std::isfinite(requestedWidth) || widthLo > widthHi)
    return limits.design;

  const int width = static_cast<int>(std::round(std::clamp(requestedWidth, widthLo, widthHi)));
  const int height = static_cast<int>(std::round(static_cast<double>(width) / aspect));
  return {width, std::clamp(height, minHeight, maxHeight)};
}

} // namespace webview
} // namespace iplug
