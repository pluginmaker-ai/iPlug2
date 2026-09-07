// Compile the same production control for another API in the same host.
#define OBJC_PREFIX ResizeProbe
#define VST3_API
#include "../IPlug/IPlugOBJCPrefix.pch"
#include "../IPlug/Extras/WebView/IPlugWebViewResizeHandle.h"

NSView* CreateVST3ResizeHandle(NSView* pTarget, iplug::webview::CornerResizeLimits limits)
{
  return [[IPLUG_RESIZE_HANDLE alloc] initWithTarget:pTarget limits:limits];
}
