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

#ifdef OS_WIN
  {
    FILE* f = fopen("C:\\temp\\iplug-resize.log", "w");
    if (f) {
      SYSTEMTIME st; GetLocalTime(&st);
      fprintf(f, "[%02d:%02d:%02d.%03d][OpenWindow] pParent=%p editorW=%d editorH=%d designW=%d designH=%d\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        pParent, GetEditorWidth(), GetEditorHeight(), mDesignWidth, mDesignHeight);
      fflush(f); fclose(f);
    }
  }
#endif

  mView = OpenWebView(pParent, 0.0f, 0.0f, static_cast<float>(GetEditorWidth()), static_cast<float>(GetEditorHeight()), 1.0f);
  return mView;
}

void WebViewEditorDelegate::Resize(int width, int height)
{
  float zoomFactor = 1.f;
  if (mNeedsDpiZoomCompensation) zoomFactor = -1.f;
  else if (mScreenScale > 1.f) zoomFactor = -2.f;
  SetWebViewBounds(0, 0, static_cast<float>(width), static_cast<float>(height), zoomFactor);

  float effectiveWidth = static_cast<float>(width);
  if (!mNeedsDpiZoomCompensation && mScreenScale > 1.f)
    effectiveWidth /= mScreenScale;

  float scale = (mDesignWidth > 0) ? (effectiveWidth / static_cast<float>(mDesignWidth)) : 1.f;
  char js[512];
  snprintf(js, sizeof(js),
    "document.documentElement.style.width='%dpx';"
    "document.documentElement.style.height='%dpx';"
    "document.documentElement.style.overflow='hidden';"
    "document.documentElement.style.transform='scale(%f)';"
    "document.documentElement.style.transformOrigin='top left';",
    mDesignWidth, mDesignHeight, scale);
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
  if (!mNeedsDpiZoomCompensation && mScreenScale > 1.f)
    effectiveWidth /= mScreenScale;

  float scale = (mDesignWidth > 0) ? (effectiveWidth / static_cast<float>(mDesignWidth)) : 1.f;

#ifdef OS_WIN
  {
    FILE* f = fopen("C:\\temp\\iplug-resize.log", "a");
    if (f) {
      SYSTEMTIME st; GetLocalTime(&st);
      fprintf(f, "[%02d:%02d:%02d.%03d][OnParentWindowResize] w=%d h=%d effectiveW=%.0f designW=%d cssScale=%.3f screenScale=%.2f needsComp=%d zoomFactor=%.2f\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        width, height, effectiveWidth, mDesignWidth, scale, mScreenScale, mNeedsDpiZoomCompensation, zoomFactor);
      fflush(f); fclose(f);
    }
  }
#endif
  char js[512];
  snprintf(js, sizeof(js),
    "document.documentElement.style.width='%dpx';"
    "document.documentElement.style.height='%dpx';"
    "document.documentElement.style.overflow='hidden';"
    "document.documentElement.style.transform='scale(%f)';"
    "document.documentElement.style.transformOrigin='top left';",
    mDesignWidth, mDesignHeight, scale);
  EvaluateJavaScript(js, nullptr);

  // Query full browser state for debugging
  EvaluateJavaScript(
    "try { var cs = getComputedStyle(document.documentElement);"
    "IPlugSendMsg({msg:'RESIZE_DEBUG', data: {"
    "  innerW: window.innerWidth,"
    "  innerH: window.innerHeight,"
    "  dpr: window.devicePixelRatio,"
    "  docOffW: document.documentElement.offsetWidth,"
    "  docOffH: document.documentElement.offsetHeight,"
    "  docScrollW: document.documentElement.scrollWidth,"
    "  docScrollH: document.documentElement.scrollHeight,"
    "  bodyScrollW: document.body ? document.body.scrollWidth : -1,"
    "  bodyScrollH: document.body ? document.body.scrollHeight : -1,"
    "  csWidth: cs.width,"
    "  csHeight: cs.height,"
    "  csTransform: cs.transform,"
    "  csOverflow: cs.overflow"
    "}}); } catch(e) {}",
    nullptr);

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