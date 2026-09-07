 /*
 ==============================================================================

  MIT License

  iPlug2 WebView Library
  Copyright (c) 2024 Oliver Larkin

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

 ==============================================================================
*/

// PluginMaker alteration: bounded corner dragging with a fixed 24-point hit area.

#pragma once

#import <AppKit/AppKit.h>
#include "IPlugWebViewCornerResize.h"
#include <functional>

// Native resize handle — sits outside CSS transform so it's always visible and draggable.
// Dragging calls setFrameSize: on the helper view, which posts NSViewFrameDidChangeNotification
// so AU hosts (Logic Pro) resize their container to match.
@interface IPLUG_RESIZE_HANDLE : NSView
{
  NSPoint mDragStart;
  NSSize mSizeAtDragStart;
  iplug::webview::CornerResizeLimits mLimits;
  NSView* mTargetView;
  std::function<void(iplug::webview::CornerSize)> mResizeRequest;
}
- (id) initWithTarget:(NSView*)target limits:(iplug::webview::CornerResizeLimits)limits;
- (void) resizeTargetToWidth:(CGFloat)width;
- (void) setResizeRequest:(std::function<void(iplug::webview::CornerSize)>)request;
@end

@implementation IPLUG_RESIZE_HANDLE

- (id) initWithTarget:(NSView*)target limits:(iplug::webview::CornerResizeLimits)limits
{
  CGFloat handleSize = 24.0;
  NSRect frame = NSMakeRect(target.frame.size.width - handleSize,
                            0,
                            handleSize, handleSize);
  self = [super initWithFrame:frame];
  if (self)
  {
    mTargetView = target;
    mLimits = limits;
    self.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
  }
  return self;
}

- (BOOL) isFlipped { return YES; }

- (void) drawRect:(NSRect)dirtyRect
{
  CGFloat w = self.bounds.size.width;
  CGFloat h = self.bounds.size.height;

  // Draw standard resize grip: three diagonal lines ⟍
  // With isFlipped=YES, (0,0) is top-left, (w,h) is bottom-right.
  // Use paired dark + light strokes for contrast on any background.
  NSBezierPath* path = [NSBezierPath bezierPath];
  [path setLineWidth:1.5];

  [path moveToPoint:NSMakePoint(w - 12, h)];
  [path lineToPoint:NSMakePoint(w, h - 12)];

  [path moveToPoint:NSMakePoint(w - 8, h)];
  [path lineToPoint:NSMakePoint(w, h - 8)];

  [path moveToPoint:NSMakePoint(w - 4, h)];
  [path lineToPoint:NSMakePoint(w, h - 4)];

  // Dark stroke first, slightly offset for a subtle drop-shadow effect that
  // gives readable contrast against light plugin backgrounds.
  [[NSColor colorWithWhite:0.0 alpha:0.45] setStroke];
  NSAffineTransform* offset = [NSAffineTransform transform];
  [offset translateXBy:1.0 yBy:1.0];
  NSBezierPath* shadow = [path copy];
  [shadow transformUsingAffineTransform:offset];
  [shadow stroke];
  [shadow release];

  // Light stroke on top — visible against dark backgrounds.
  [[NSColor colorWithWhite:1.0 alpha:0.6] setStroke];
  [path stroke];
}

- (void) mouseDown:(NSEvent*)event
{
  mDragStart = [mTargetView.window convertPointToScreen:event.locationInWindow];
  mSizeAtDragStart = mTargetView.frame.size;
}

- (void) mouseDragged:(NSEvent*)event
{
  // Use this event's position, not a later global cursor sample. Screen
  // coordinates stay stable when the AU host moves its window during a drag.
  NSPoint current = [mTargetView.window convertPointToScreen:event.locationInWindow];
  CGFloat dx = current.x - mDragStart.x;
  [self resizeTargetToWidth:mSizeAtDragStart.width + dx];
}

- (void) resizeTargetToWidth:(CGFloat)width
{
  const auto size = iplug::webview::ConstrainCornerWidth(width, mLimits);
  if (mResizeRequest)
  {
    // VST3 asks its host first. Only the host's onSize callback commits the
    // frame and size memory, including when it defers or rejects the request.
    mResizeRequest(size);
    return;
  }
  // AU hosts observe this frame change. Apply one bounded frame, so the host
  // and embedded page never receive conflicting sizes during the callback.
  [mTargetView setFrameSize:NSMakeSize(size.width, size.height)];
}

- (void) setResizeRequest:(std::function<void(iplug::webview::CornerSize)>)request
{
  mResizeRequest = std::move(request);
}

- (BOOL) acceptsFirstMouse:(NSEvent*)event
{
  return YES;
}

- (void) resetCursorRects
{
  // Use private API for diagonal resize cursor (nwse), fall back to arrow
  NSCursor* resizeCursor = nil;
  if ([NSCursor respondsToSelector:@selector(_windowResizeNorthWestSouthEastCursor)])
    resizeCursor = [NSCursor performSelector:@selector(_windowResizeNorthWestSouthEastCursor)];
  [self addCursorRect:self.bounds cursor:(resizeCursor ?: [NSCursor arrowCursor])];
}

@end
