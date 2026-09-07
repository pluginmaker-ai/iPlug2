#define IPLUG_RESIZE_HANDLE IPlugTestResizeHandle
#include "../IPlug/Extras/WebView/IPlugWebViewResizeHandle.h"

#include <cassert>
#include <iostream>
#include <vector>

@interface ResizeObserver : NSObject
{
@public
  std::vector<NSSize> mFrames;
}
- (void) frameChanged:(NSNotification*)notification;
@end

@implementation ResizeObserver
- (void) frameChanged:(NSNotification*)notification
{
  mFrames.push_back([(NSView*)notification.object frame].size);
}
@end

static void CheckFrame(NSView* pTarget, IPlugTestResizeHandle* pHandle, NSSize expected)
{
  assert(NSEqualSizes(pTarget.frame.size, expected));
  assert(NSEqualSizes(pHandle.frame.size, NSMakeSize(24, 24)));
  assert(pHandle.frame.origin.x == expected.width - 24);
  assert(pHandle.frame.origin.y == 0);
  assert([pTarget hitTest:NSMakePoint(expected.width - 23, 23)] == pHandle);
  assert([pTarget hitTest:NSMakePoint(expected.width - 1, 1)] == pHandle);
}

int main()
{
  @autoreleasepool
  {
    NSView* pTarget = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 520)];
    [pTarget setPostsFrameChangedNotifications:YES];
    ResizeObserver* pObserver = [[ResizeObserver alloc] init];
    [[NSNotificationCenter defaultCenter] addObserver:pObserver selector:@selector(frameChanged:)
                                                name:NSViewFrameDidChangeNotification object:pTarget];
    const iplug::webview::CornerResizeLimits limits{{800, 520}, {400, 300}, {3200, 2400}};
    IPlugTestResizeHandle* pHandle = [[IPlugTestResizeHandle alloc] initWithTarget:pTarget limits:limits];
    [pTarget addSubview:pHandle positioned:NSWindowAbove relativeTo:nil];
    CheckFrame(pTarget, pHandle, NSMakeSize(800, 520));

    for (int repeat = 0; repeat < 10; ++repeat)
    {
      [pHandle resizeTargetToWidth:200];
      CheckFrame(pTarget, pHandle, NSMakeSize(462, 300));
      [pHandle resizeTargetToWidth:1120];
      CheckFrame(pTarget, pHandle, NSMakeSize(1120, 728));
      [pHandle resizeTargetToWidth:10000];
      CheckFrame(pTarget, pHandle, NSMakeSize(3200, 2080));
    }

    // The AU host's notification path sees only accepted dimensions; there is
    // no transient 200-point frame followed by a callback correction.
    assert(pObserver->mFrames.size() == 30);
    for (const NSSize size : pObserver->mFrames)
    {
      assert(size.width >= 400 && size.height >= 300);
      assert(size.width <= 3200 && size.height <= 2400);
    }

    [[NSNotificationCenter defaultCenter] removeObserver:pObserver];
    [pHandle release];
    [pTarget release];
    [pObserver release];
  }
  std::cout << "IPlugWebViewResizeHandleTest passed\n";
}
