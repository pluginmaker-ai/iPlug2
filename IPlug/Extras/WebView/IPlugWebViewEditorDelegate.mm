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

#if __has_feature(objc_arc)
#error This file must be compiled without Arc. Don't use -fobjc-arc flag!
#endif

#include "IPlugWebViewEditorDelegate.h"

#ifdef OS_MAC
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#elif defined(OS_IOS)
#import <UIKit/UIKit.h>
#endif

#if defined OS_MAC
  #define PLATFORM_VIEW NSView
#elif defined OS_IOS
  #define PLATFORM_VIEW UIView
#endif

using namespace iplug;

@interface IPLUG_WKWEBVIEW_EDITOR_HELPER : PLATFORM_VIEW
{
  WebViewEditorDelegate* mDelegate;
}
- (void) removeFromSuperview;
- (id) initWithEditorDelegate: (WebViewEditorDelegate*) pDelegate;
@end

#ifdef OS_MAC
// Native resize handle — sits outside CSS transform so it's always visible and draggable.
// Dragging calls setFrameSize: on the helper view, which posts NSViewFrameDidChangeNotification
// so AU hosts (Logic Pro) resize their container to match.
@interface IPLUG_RESIZE_HANDLE : NSView
{
  NSPoint mDragStart;
  NSSize mSizeAtDragStart;
  CGFloat mAspectRatio;
  NSView* mTargetView;
}
- (id) initWithTarget:(NSView*)target aspectRatio:(CGFloat)ratio;
@end

@implementation IPLUG_RESIZE_HANDLE

- (id) initWithTarget:(NSView*)target aspectRatio:(CGFloat)ratio
{
  CGFloat handleSize = 16.0;
  NSRect frame = NSMakeRect(target.frame.size.width - handleSize,
                            0,
                            handleSize, handleSize);
  self = [super initWithFrame:frame];
  if (self)
  {
    mTargetView = target;
    mAspectRatio = ratio;
    self.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
  }
  return self;
}

- (void) drawRect:(NSRect)dirtyRect
{
  NSRect bounds = self.bounds;
  CGFloat w = bounds.size.width;
  CGFloat h = bounds.size.height;

  [[NSColor colorWithWhite:1.0 alpha:0.3] setStroke];
  NSBezierPath* path = [NSBezierPath bezierPath];
  [path setLineWidth:1.5];

  // macOS Y is flipped (0 = bottom). Lines go from bottom-right toward top-left.
  [path moveToPoint:NSMakePoint(w - 2, 2)];
  [path lineToPoint:NSMakePoint(2, h - 2)];

  [path moveToPoint:NSMakePoint(w - 2, h * 0.25 + 2)];
  [path lineToPoint:NSMakePoint(w * 0.25 + 2, h - 2)];

  [path moveToPoint:NSMakePoint(w - 2, h * 0.5 + 2)];
  [path lineToPoint:NSMakePoint(w * 0.5 + 2, h - 2)];

  [path stroke];
}

- (void) mouseDown:(NSEvent*)event
{
  mDragStart = [NSEvent mouseLocation];
  mSizeAtDragStart = mTargetView.frame.size;
}

- (void) mouseDragged:(NSEvent*)event
{
  NSPoint current = [NSEvent mouseLocation];
  CGFloat dx = current.x - mDragStart.x;
  // macOS y is flipped — dragging down = negative dy, but we want width to grow
  CGFloat newWidth = MAX(200.0, mSizeAtDragStart.width + dx);
  CGFloat newHeight = round(newWidth / mAspectRatio);

  [mTargetView setFrameSize:NSMakeSize(newWidth, newHeight)];
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
#endif

@implementation IPLUG_WKWEBVIEW_EDITOR_HELPER
{
  BOOL mInResize;
}

- (id) initWithEditorDelegate: (WebViewEditorDelegate*) pDelegate;
{
  mDelegate = pDelegate;
  CGFloat w = pDelegate->GetEditorWidth();
  CGFloat h = pDelegate->GetEditorHeight();
  CGRect r = CGRectMake(0, 0, w, h);
  self = [super initWithFrame:r];

  void* pWebView = pDelegate->OpenWebView(self, 0, 0, w, h, 1.0f);

  [self addSubview: (PLATFORM_VIEW*) pWebView];

#ifdef OS_MAC
  // Add native resize handle on top of the WebView
  if (w > 0 && h > 0)
  {
    IPLUG_RESIZE_HANDLE* resizeHandle = [[IPLUG_RESIZE_HANDLE alloc] initWithTarget:self aspectRatio:(w / h)];
    [self addSubview:resizeHandle positioned:NSWindowAbove relativeTo:nil];
  }
#endif

  return self;
}

- (void) removeFromSuperview
{
#ifdef AU_API
  //For AUv2 this is where we know about the window being closed, close via delegate
  mDelegate->CloseWindow();
#endif
  [super removeFromSuperview];
}

#ifdef OS_MAC
- (void) viewDidMoveToWindow
{
  [super viewDidMoveToWindow];
  if (self.window && mDelegate)
  {
    CGFloat w = mDelegate->GetEditorWidth();
    CGFloat h = mDelegate->GetEditorHeight();
    if (w > 0 && h > 0)
      [self.window setContentAspectRatio:NSMakeSize(w, h)];
  }
}

- (void) setFrameSize:(NSSize)newSize
{
  [super setFrameSize:newSize];
  if (mDelegate && !mInResize)
  {
    mInResize = YES;
    mDelegate->OnParentWindowResize(static_cast<int>(newSize.width), static_cast<int>(newSize.height));
    mInResize = NO;
  }
}
#endif

@end

WebViewEditorDelegate::WebViewEditorDelegate(int nParams)
: IEditorDelegate(nParams)
, IWebView()
{
}

WebViewEditorDelegate::~WebViewEditorDelegate()
{
  CloseWindow();
  
  PLATFORM_VIEW* pHelperView = (PLATFORM_VIEW*) mView;
  [pHelperView release];
  mView = nullptr;
}

void* WebViewEditorDelegate::OpenWindow(void* pParent)
{
  PLATFORM_VIEW* pParentView = (PLATFORM_VIEW*) pParent;

  if (mDesignWidth == 0) {
    mDesignWidth = GetEditorWidth();
    mDesignHeight = GetEditorHeight();
  }

  IPLUG_WKWEBVIEW_EDITOR_HELPER* pHelperView = [[IPLUG_WKWEBVIEW_EDITOR_HELPER alloc] initWithEditorDelegate: this];
  mView = (void*) pHelperView;

  if (pParentView)
  {
    [pParentView addSubview: pHelperView];
  }
  
  if (mEditorInitFunc)
  {
    mEditorInitFunc();
  }
  
  return mView;
}

void WebViewEditorDelegate::Resize(int width, int height)
{
  ResizeWebViewAndHelper(width, height);
  EditorResizeFromUI(width, height, true);
}

void WebViewEditorDelegate::OnParentWindowResize(int width, int height)
{
  ResizeWebViewAndHelper(width, height);
  EditorResizeFromUI(width, height, false);
}

void WebViewEditorDelegate::ResizeWebViewAndHelper(float width, float height)
{
  CGFloat w = static_cast<float>(width);
  CGFloat h = static_cast<float>(height);
  IPLUG_WKWEBVIEW_EDITOR_HELPER* pHelperView = (IPLUG_WKWEBVIEW_EDITOR_HELPER*) mView;
  [pHelperView setFrame:CGRectMake(0, 0, w, h)];

  // Use CSS transform for uniform scaling — works for both rendering AND hit testing.
  // The WKWebView fills the window, but CSS locks the layout to design dimensions
  // and visually scales to fit.
  float scale = (mDesignWidth > 0) ? (w / static_cast<float>(mDesignWidth)) : 1.f;
  SetWebViewBounds(0, 0, w, h, 1.f);

  char js[512];
  snprintf(js, sizeof(js),
    "document.documentElement.style.width='%dpx';"
    "document.documentElement.style.height='%dpx';"
    "document.documentElement.style.overflow='hidden';"
    "document.documentElement.style.transform='scale(%f)';"
    "document.documentElement.style.transformOrigin='top left';",
    mDesignWidth, mDesignHeight, scale);
  EvaluateJavaScript(js, nullptr);
}

bool WebViewEditorDelegate::OnKeyDown(const IKeyPress& key)
{
  return false;
}

bool WebViewEditorDelegate::OnKeyUp(const IKeyPress& key)
{
  return false;
}
