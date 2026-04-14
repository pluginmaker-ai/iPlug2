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

#include "IPlugWebViewEditorDelegate.h"

using namespace iplug;

WebViewEditorDelegate::WebViewEditorDelegate(int nParams)
  : IEditorDelegate(nParams)
  , IWebView()
{
}

WebViewEditorDelegate::~WebViewEditorDelegate()
{
  CloseWindow();
}

void* WebViewEditorDelegate::OpenWindow(void* pParent)
{
  if (mDesignWidth == 0) {
    mDesignWidth = GetEditorWidth();
    mDesignHeight = GetEditorHeight();
  }

  mView = OpenWebView(pParent, 0.0f, 0.0f, static_cast<float>(GetEditorWidth()), static_cast<float>(GetEditorHeight()), 1.0f);

  // Plumb min-size constraints through to the Win32 WM_SIZING hook so the
  // aspect-ratio-locked drag clamps at the plugin's configured minimum
  // instead of shrinking past it.
  SetWindowsMinSize(GetMinWidth(), GetMinHeight());

  return mView;
}

void WebViewEditorDelegate::Resize(int width, int height)
{
  float zoomFactor = 1.f;
  if (mNeedsDpiZoomCompensation) zoomFactor = -1.f;
  else if (mScreenScale > 1.f) zoomFactor = -2.f;
  SetWebViewBounds(0, 0, static_cast<float>(width), static_cast<float>(height), zoomFactor);

  float effectiveWidth = static_cast<float>(width);
  float effectiveHeight = static_cast<float>(height);
  if (!mNeedsDpiZoomCompensation && mScreenScale > 1.f)
  {
    effectiveWidth /= mScreenScale;
    effectiveHeight /= mScreenScale;
  }

  float scaleX = (mDesignWidth > 0) ? (effectiveWidth / static_cast<float>(mDesignWidth)) : 1.f;
  float scaleY = (mDesignHeight > 0) ? (effectiveHeight / static_cast<float>(mDesignHeight)) : 1.f;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;
  char js[1024];
  snprintf(js, sizeof(js),
    "document.documentElement.style.width='%dpx';"
    "document.documentElement.style.height='%dpx';"
    "document.documentElement.style.overflow='hidden';"
    "document.documentElement.style.transform='scale(%f)';"
    "document.documentElement.style.transformOrigin='top left';"
    "document.body.style.width='%dpx';"
    "document.body.style.height='%dpx';"
    "document.body.style.position='relative';"
    "document.body.style.overflow='hidden';",
    mDesignWidth, mDesignHeight, scale, mDesignWidth, mDesignHeight);
  EvaluateJavaScript(js, nullptr);

  EditorResizeFromUI(width, height, true);
}

void WebViewEditorDelegate::OnParentWindowResize(int width, int height)
{
  // mNeedsDpiZoomCompensation == true: FL Studio — pass -1 to trigger 1/dpiScale zoom
  // mNeedsDpiZoomCompensation == false but mScreenScale > 1: Cubase — pass -2 to skip DPI rect scaling
  // Otherwise: Ableton/default — pass 1.0 (original behavior)
  // FL Studio: -1 → zoom = 1/dpiScale, normal DPI bounds
  // Cubase: -2 → zoom = 1.0, skip DPI bounds scaling (host sends physical pixels)
  // Ableton/default: 1.0 → original behavior
  float zoomFactor = 1.f;
  if (mNeedsDpiZoomCompensation) zoomFactor = -1.f;
  else if (mScreenScale > 1.f) zoomFactor = -2.f;

  SetWebViewBounds(0, 0, static_cast<float>(width), static_cast<float>(height), zoomFactor);

  // For hosts that send physical pixels (Cubase), divide by screenScale to get
  // logical CSS scale. For hosts that send logical pixels (FL Studio, Ableton),
  // screenScale is 1.0 or zoom compensation handles it — no division needed.
  float effectiveWidth = static_cast<float>(width);
  float effectiveHeight = static_cast<float>(height);
  if (!mNeedsDpiZoomCompensation && mScreenScale > 1.f)
  {
    effectiveWidth /= mScreenScale;
    effectiveHeight /= mScreenScale;
  }

  float scaleX = (mDesignWidth > 0) ? (effectiveWidth / static_cast<float>(mDesignWidth)) : 1.f;
  float scaleY = (mDesignHeight > 0) ? (effectiveHeight / static_cast<float>(mDesignHeight)) : 1.f;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;

  char js[1024];
  snprintf(js, sizeof(js),
    "document.documentElement.style.width='%dpx';"
    "document.documentElement.style.height='%dpx';"
    "document.documentElement.style.overflow='hidden';"
    "document.documentElement.style.transform='scale(%f)';"
    "document.documentElement.style.transformOrigin='top left';"
    "document.body.style.width='%dpx';"
    "document.body.style.height='%dpx';"
    "document.body.style.position='relative';"
    "document.body.style.overflow='hidden';",
    mDesignWidth, mDesignHeight, scale, mDesignWidth, mDesignHeight);
  EvaluateJavaScript(js, nullptr);

  EditorResizeFromUI(width, height, false);
}

bool WebViewEditorDelegate::OnKeyDown(const IKeyPress& key)
{
  #ifdef OS_WIN
  if (key.VK == VK_SPACE)
  {
    PostMessage((HWND)mView, WM_KEYDOWN, VK_SPACE, 0);
    return true;
  }
  #endif
  return false;
}

bool WebViewEditorDelegate::OnKeyUp(const IKeyPress& key)
{

  return true;
}