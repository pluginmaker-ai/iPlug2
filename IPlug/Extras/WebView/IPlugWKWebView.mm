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

#import "IPlugWKWebView.h"

#include "IPlugPlatform.h"

@implementation IPLUG_WKWEBVIEW

- (instancetype)initWithFrame:(CGRect)frame configuration:(WKWebViewConfiguration *)configuration
{
  self = [super initWithFrame:frame configuration:configuration];

  if (self)
  {
    mEnableInteraction = true;
#ifdef OS_MAC
    // Capture the most recent left-mousedown that lands inside our window so
    // beginDraggingSessionWithItems:event:source: has a fresh event when JS
    // requests a file drag. WKWebView's content view eats mouseDown: before
    // this subclass can override it, so a local NSEvent monitor is the only
    // reliable place to capture the original gesture. The monitor returns the
    // event unchanged so WKWebView's own handling is undisturbed.
    __weak typeof(self) weakSelf = self;
    mEventMonitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp)
                                   handler:^NSEvent* _Nullable(NSEvent* event) {
      typeof(self) strongSelf = weakSelf;
      if (!strongSelf) return event;
      if (event.window == strongSelf.window)
      {
        if (event.type == NSEventTypeLeftMouseDown)
          strongSelf->mLastMouseDownEvent = event;
        else
          strongSelf->mLastMouseDownEvent = nil;
      }
      return event;
    }];
#endif
  }
  return self;
}

#ifdef OS_MAC
- (void)dealloc
{
  if (mEventMonitor)
  {
    [NSEvent removeMonitor:mEventMonitor];
    mEventMonitor = nil;
  }
  mLastMouseDownEvent = nil;
}

- (BOOL)startFileDragWithPath:(NSString*)path
{
  if (!mLastMouseDownEvent || path.length == 0)
    return NO;

  NSURL* fileURL = [NSURL fileURLWithPath:path];
  if (!fileURL || ![[NSFileManager defaultManager] fileExistsAtPath:path])
    return NO;

  NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
  // setString:forType: with NSPasteboardTypeFileURL requires the URL
  // string form ("file:///foo.mid"), not the bare path — most DAWs route
  // file drops through this exact pasteboard type, including Logic.
  [item setString:[fileURL absoluteString] forType:NSPasteboardTypeFileURL];

  NSDraggingItem* draggingItem = [[NSDraggingItem alloc] initWithPasteboardWriter:item];

  // Anchor the drag image at the captured mousedown location (in our
  // coordinate space) so the icon doesn't snap to the corner when the
  // session starts. Use the system's icon for the file's extension so
  // the user sees a recognizable preview (e.g. a MIDI file icon).
  NSPoint locationInView = [self convertPoint:[mLastMouseDownEvent locationInWindow] fromView:nil];
  NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFileType:[fileURL pathExtension]];
  static const CGFloat kDragIconSize = 32.0;
  NSRect dragFrame = NSMakeRect(locationInView.x - kDragIconSize / 2.0,
                                locationInView.y - kDragIconSize / 2.0,
                                kDragIconSize,
                                kDragIconSize);
  [draggingItem setDraggingFrame:dragFrame contents:icon];

  [self beginDraggingSessionWithItems:@[draggingItem]
                                event:mLastMouseDownEvent
                               source:self];
  return YES;
}

#pragma mark - NSDraggingSource

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
  // NSDraggingContextOutsideApplication is the only one that matters here
  // — the user wants to drop into the host DAW. Inside-application drags
  // (rare for a plugin window) are also Copy so the same operation reads
  // both ways.
  return NSDragOperationCopy;
}

- (NSView *)hitTest:(NSPoint)point
{
  if (!mEnableInteraction)
  {
    return nil;
  }
  else
    return [super hitTest:point];
}

- (void)willOpenMenu:(NSMenu *)menu withEvent:(NSEvent *)event
{
  [super willOpenMenu:menu withEvent:event];
  
  NSArray<NSString *> *WKStrings = @[
   @"WKMenuItemIdentifierCopy",
   @"WKMenuItemIdentifierCopyImage",
   @"WKMenuItemIdentifierCopyLink",
   @"WKMenuItemIdentifierDownloadImage",
   @"WKMenuItemIdentifierDownloadLinkedFile",
   @"WKMenuItemIdentifierGoBack",
   @"WKMenuItemIdentifierGoForward",
//   @"WKMenuItemIdentifierInspectElement",
   @"WKMenuItemIdentifierLookUp",
   @"WKMenuItemIdentifierOpenFrameInNewWindow",
   @"WKMenuItemIdentifierOpenImageInNewWindow",
   @"WKMenuItemIdentifierOpenLink",
   @"WKMenuItemIdentifierOpenLinkInNewWindow",
   @"WKMenuItemIdentifierPaste",
//   @"WKMenuItemIdentifierReload",
   @"WKMenuItemIdentifierSearchWeb",
   @"WKMenuItemIdentifierShowHideMediaControls",
   @"WKMenuItemIdentifierToggleFullScreen",
   @"WKMenuItemIdentifierTranslate",
   @"WKMenuItemIdentifierShareMenu",
   @"WKMenuItemIdentifierSpeechMenu"
  ];
  
  for (NSInteger itemIndex = 0; itemIndex < menu.itemArray.count; itemIndex++)
  {
    if ([WKStrings containsObject:menu.itemArray[itemIndex].identifier])
    {
      [menu removeItemAtIndex:itemIndex];
    }
  }
}

#endif

- (void)setEnableInteraction:(bool)enable
{
  mEnableInteraction = enable;
  
#ifdef OS_MAC
  if (!mEnableInteraction)
  {
    for (NSTrackingArea* trackingArea in self.trackingAreas)
    {
      [self removeTrackingArea:trackingArea];
    }
  }
#else
  self.userInteractionEnabled = mEnableInteraction;
#endif
}

- (BOOL)allowsLinkPreview
{
  return false;
}

- (BOOL)acceptsFirstResponder {
    return NO;
}

#ifdef OS_MAC
- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (([event modifierFlags] & NSEventModifierFlagCommand) && 
        !([event modifierFlags] & NSEventModifierFlagShift) && 
        !([event modifierFlags] & NSEventModifierFlagOption) && 
        !([event modifierFlags] & NSEventModifierFlagControl)) {
        
        NSString *characters = [event charactersIgnoringModifiers];
        if ([characters length] == 1) {
            unichar character = [characters characterAtIndex:0];
            if (character == 'q' || character == 'w' || character == 'h' || 
                character == 's' || character == 'a' || character == 'z' || 
                character == 'x' || character == 'c' || character == 'v' ||
                character == ',') {
                return NO;
            }
        }
    }
    
    return YES;
}
#endif
@end
