#include "../IPlug/Extras/WebView/IPlugWebViewCornerResize.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using namespace iplug::webview;

static void CheckSize(CornerSize actual, CornerSize expected)
{
  assert(actual.width == expected.width);
  assert(actual.height == expected.height);
}

static void CheckBounds(CornerSize design)
{
  const CornerResizeLimits limits{design, {400, 300}, {3200, 2400}};
  for (int requested = -1000; requested <= 12000; requested += 7)
  {
    const auto size = ConstrainCornerWidth(requested, limits);
    assert(size.width >= std::min(400, design.width));
    assert(size.height >= std::min(300, design.height));
    assert(size.width <= std::max(3200, design.width));
    assert(size.height <= std::max(2400, design.height));
    const double expectedHeight = static_cast<double>(size.width) * design.height / design.width;
    assert(std::abs(size.height - expectedHeight) <= 0.500001);
    CheckSize(ConstrainCornerWidth(size.width, limits), size);
  }
}

int main()
{
  const CornerResizeLimits standard{{800, 520}, {400, 300}, {3200, 2400}};
  CheckSize(ConstrainCornerWidth(200, standard), {462, 300});
  CheckSize(ConstrainCornerWidth(-400, standard), {462, 300});
  CheckSize(ConstrainCornerWidth(1120, standard), {1120, 728});
  CheckSize(ConstrainCornerWidth(99999, standard), {3200, 2080});
  CheckSize(ConstrainCornerWidth(800.4, standard), {800, 520});
  CheckSize(ConstrainCornerWidth(std::numeric_limits<double>::infinity(), standard), {800, 520});
  CheckSize(ConstrainCornerWidth(std::numeric_limits<double>::quiet_NaN(), standard), {800, 520});

  for (const CornerSize design : {CornerSize{800, 520}, {1400, 920}, {1200, 800},
                                  {256, 256}, {8192, 256}, {256, 8192}, {701, 333}})
    CheckBounds(design);

  // Restoring a small custom design must not silently impose a larger floor.
  CheckSize(ConstrainCornerWidth(100, {{256, 256}, {400, 300}, {3200, 2400}}), {256, 256});
  CheckSize(ConstrainCornerWidth(200, {{800, 520}, {0, 0}, {0, 0}}), {200, 130});
  CheckSize(ConstrainCornerWidth(200, {{0, 0}, {400, 300}, {3200, 2400}}), {400, 300});
  std::cout << "IPlugWebViewCornerResizeTest passed\n";
}
