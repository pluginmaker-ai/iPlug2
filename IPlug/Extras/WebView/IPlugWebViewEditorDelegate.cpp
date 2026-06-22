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
    // Forward spacebar to the DAW so transport (play/stop) still works while
    // the WebView is focused.
    //
    // Why SendInput (and not just PostMessage WM_KEYDOWN or WM_APPCOMMAND):
    // empirical testing in Ableton Live on Windows confirms that
    // PostMessage(GA_ROOT, WM_KEYDOWN, VK_SPACE, lParam) does NOT toggle
    // transport, even with a real scan code in lParam; and Ableton ignores
    // PostMessage(GA_ROOT, WM_APPCOMMAND, MEDIA_PLAY_PAUSE) too. Ableton
    // appears to bind its transport via either RegisterHotKey (delivers
    // WM_HOTKEY for real OS keypresses only — synthesized PostMessage'd
    // WM_KEYDOWN does not generate WM_HOTKEY) or raw input / a focus-gated
    // accelerator filter on a specific child window that is not GA_ROOT.
    //
    // The only path that goes through the same OS input pipeline as a real
    // hardware keystroke is SendInput. But SendInput delivers to whichever
    // window has keyboard focus AT THE TIME the OS dispatches the queued
    // input — and that's our WebView, not the DAW. To make the synthesized
    // key actually reach the DAW, we briefly transfer focus to the host's
    // top-level window before SendInput, pump messages so the OS can drain
    // the input queue while the DAW still has focus, then restore focus to
    // whatever had it before.
    //
    // Trade-offs:
    //  - The DAW gets focus for ~30ms. Imperceptible to the user; no
    //    visible flicker because GA_ROOT is already the active top-level.
    //  - We re-enter our own message loop briefly. Other messages may run
    //    during this window (timers, paint). Bounded to 30ms.
    //  - Synthesized space WILL echo back to the focused WebView once we
    //    restore focus. The 150ms tick guard absorbs the echo.
    //  - Foreground-process check ensures we don't fire space into Discord
    //    or a browser if the user alt-tabbed mid-event. The TOCTOU race
    //    window (check -> SetFocus -> SendInput) is microseconds.
    //
    // macOS is untouched — its NSResponder chain handles this naturally
    // and the .mm code path returns false on space.

    const unsigned int nowMs = (unsigned int) GetTickCount();
    if (nowMs - mLastSpaceForwardMs < 150)
      return true; // recursion / echo guard
    mLastSpaceForwardMs = nowMs;

    HWND root = GetAncestor((HWND) mView, GA_ROOT);
    if (!root || root == (HWND) mView)
      return true;

    // Foreground guard — only fire if our plugin's process owns the
    // foreground window. Without this, alt-tabbing during a held space
    // would dump synth space presses into whatever app is now in front.
    HWND fg = GetForegroundWindow();
    if (!fg)
      return true;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid != GetCurrentProcessId())
      return true;

    // Allow cross-thread SetFocus / focus state sharing if the DAW's UI
    // runs on a different thread than ours. In most VST3 hosts the plugin
    // editor shares the host UI thread, so this is a no-op.
    DWORD ourThread = GetCurrentThreadId();
    DWORD rootThread = GetWindowThreadProcessId(root, nullptr);
    BOOL attached = FALSE;
    if (ourThread != rootThread)
      attached = AttachThreadInput(ourThread, rootThread, TRUE);

    HWND prevFocus = GetFocus();
    SetFocus(root);

    // Synthesize a real OS-level SPACE keystroke. Goes through the full
    // Windows input pipeline (raw input, WM_HOTKEY, accelerator tables) —
    // unlike PostMessage which is invisible to RegisterHotKey filters.
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_SPACE;
    in[0].ki.wScan = (WORD) MapVirtualKey(VK_SPACE, MAPVK_VK_TO_VSC);
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));

    // Pump messages so the DAW's WndProc runs while focus is still on root
    // and processes the synth WM_KEYDOWN/WM_KEYUP that the OS just queued.
    // If we returned without pumping, our caller's message loop would
    // resume, but by then we'd already have restored focus to the WebView
    // — and the OS would dispatch the synth key to the WebView, defeating
    // the entire purpose.
    const DWORD endTick = GetTickCount() + 30;
    MSG msg;
    while ((unsigned int) GetTickCount() < endTick)
    {
      if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
      {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
      else
      {
        Sleep(1);
      }
    }

    SetFocus(prevFocus);
    if (attached)
      AttachThreadInput(ourThread, rootThread, FALSE);

    return true;
  }
  #endif
  return false;
}

bool WebViewEditorDelegate::OnKeyUp(const IKeyPress& key)
{

  return true;
}