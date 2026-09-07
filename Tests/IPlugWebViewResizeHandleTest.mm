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

static NSEvent* MouseEvent(NSView* pTarget, NSEventType type, NSPoint screenPoint)
{
  return [NSEvent mouseEventWithType:type
                           location:[pTarget.window convertPointFromScreen:screenPoint]
                      modifierFlags:0 timestamp:0 windowNumber:pTarget.window.windowNumber
                            context:nil eventNumber:0 clickCount:1 pressure:1];
}

int main()
{
  @autoreleasepool
  {
    [NSApplication sharedApplication];
    NSWindow* pHost = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 800, 520)
                                                styleMask:NSWindowStyleMaskBorderless
                                                  backing:NSBackingStoreBuffered defer:NO];
    NSView* pTarget = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 520)];
    pHost.contentView = pTarget;
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
      const NSPoint dragStart{900, 100};
      const CGFloat startWidth = pTarget.frame.size.width;
      [pHandle mouseDown:MouseEvent(pTarget, NSEventTypeLeftMouseDown, dragStart)];
      [pHandle mouseDragged:MouseEvent(pTarget, NSEventTypeLeftMouseDragged,
                                       NSMakePoint(dragStart.x + 200 - startWidth, 100))];
      CheckFrame(pTarget, pHandle, NSMakeSize(462, 300));
      // Simulate an AU host moving its window between two drag events.
      [pHost setFrameOrigin:NSMakePoint(100, 100 + repeat * 10)];
      [pHandle mouseDragged:MouseEvent(pTarget, NSEventTypeLeftMouseDragged,
                                       NSMakePoint(dragStart.x + 1120 - startWidth, 100))];
      CheckFrame(pTarget, pHandle, NSMakeSize(1120, 728));
      [pHandle mouseDragged:MouseEvent(pTarget, NSEventTypeLeftMouseDragged,
                                       NSMakePoint(dragStart.x + 10000 - startWidth, 100))];
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
    [pHost release];
    [pObserver release];
  }
  std::cout << "IPlugWebViewResizeHandleTest passed\n";
}
