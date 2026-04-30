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

#pragma once

#import <WebKit/WebKit.h>
// TargetConditionals defines TARGET_OS_OSX directly from compiler builtins
// (no dependency on whether IPlugPlatform.h was included first). iPlug2's
// own OS_MAC macro lives in IPlugPlatform.h, which most call sites import
// AFTER this header — so a `#ifdef OS_MAC` here would silently skip even
// on macOS builds. Avoid that footgun by gating on TARGET_OS_OSX, which
// is set the moment Cocoa.h / TargetConditionals.h is in scope (they're
// pulled in transitively by <WebKit/WebKit.h> above).
#include <TargetConditionals.h>

#if TARGET_OS_OSX
@interface IPLUG_WKWEBVIEW : WKWebView <NSDraggingSource>
#else
@interface IPLUG_WKWEBVIEW : WKWebView
#endif
{
  bool mEnableInteraction;
#if TARGET_OS_OSX
  // Captured by an NSEvent local monitor so we always have a fresh
  // mousedown to feed into beginDraggingSessionWithItems:event:source:
  // when JS triggers an external file drag (e.g. a drum machine MIDI
  // export). WKWebView's content view consumes mouseDown: before this
  // class can override it, and [NSApp currentEvent] is unreliable inside
  // the async WKScriptMessageHandler dispatch — the monitor is the only
  // reliable path. Cleared on mouseUp so a stale event can't start a
  // bogus drag after the user releases.
  NSEvent* _Nullable mLastMouseDownEvent;
  id _Nullable mEventMonitor;
#endif
}

- (instancetype)initWithFrame:(CGRect)frame configuration:(WKWebViewConfiguration *)configuration;

- (void)setEnableInteraction:(bool)enable;

#if TARGET_OS_OSX
- (NSView *)hitTest:(NSPoint)point;
- (void)willOpenMenu:(NSMenu *)menu withEvent:(NSEvent *)event;

/** Start an OS-level drag session with the file at `path` as a
 *  pasteboard item, so the user can drop it onto another app (DAW
 *  timeline, Finder, etc). Uses the most-recent left-mousedown captured
 *  by the local NSEvent monitor. Returns NO if no fresh mousedown is
 *  available (no active gesture) or if the path can't be resolved. */
- (BOOL)startFileDragWithPath:(NSString*)path;
#endif

@end
