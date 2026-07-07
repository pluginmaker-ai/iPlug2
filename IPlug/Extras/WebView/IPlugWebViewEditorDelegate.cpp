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

  // Fit the content to the WebView's ACTUAL measured viewport (see
  // InjectViewportFit) rather than a scale derived from mScreenScale.
  InjectViewportFit();

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

  // Fit the content to the WebView's ACTUAL measured viewport (see
  // InjectViewportFit) rather than a scale derived from mScreenScale — the old
  // path over-scaled the content by (realDPI / mScreenScale) after a mixed-DPI
  // monitor move, or when the host's reported scale didn't match the WebView's
  // rasterization, overflowing the viewport and clipping the bottom of the UI
  // (the Studio One / Fender Studio Pro high-DPI clip).
  InjectViewportFit();

  EditorResizeFromUI(width, height, false);
}

void WebViewEditorDelegate::InjectViewportFit()
{
  // Scale the design (mDesignWidth x mDesignHeight) to fit the WebView's real
  // viewport, measured live via window.innerWidth/innerHeight — this is the true
  // CSS-pixel space the content has, regardless of DPI, host, or which monitor
  // the window was dragged onto. min(iw/dw, ih/dh) is by construction the largest
  // scale at which the whole UI fits, so it can never overflow/clip; a host that
  // already sizes us correctly measures the same scale as before (no visible
  // change). Self-installs a single resize listener so the fit also re-runs on
  // drag-resize and on DPI change (moving to a different-scale monitor changes
  // innerWidth and fires 'resize'), not only when the host calls us back.
  // The fit must survive the WebView's rasterization scale settling AFTER the
  // page measured its viewport (measured on a fresh open on a 175% monitor:
  // innerWidth still reported pre-raster CSS px at NavigationCompleted, the fit
  // over-scaled, and no 'resize' event fired when the viewport later shrank
  // under it). So besides the 'resize' listener, re-fit on devicePixelRatio
  // changes via the canonical matchMedia('(resolution: Xdppx)') watcher, plus a
  // few timed convergence re-fits. The fit itself is idempotent and cheap.
  // DPI-virtualized hosts (Ableton Auto-Scale) make innerWidth/Height lie by
  // exactly the virtualization factor — the page persistently measures the
  // physical-derived viewport while only measured/vf CSS px are visible, and
  // no resize/dpr event ever corrects it (measured: iw=1500 for a visible
  // ~1200, across a whole session). The native zoom reconcile measures the
  // factor and publishes it via SetViewportVirtScale (1.0 everywhere else);
  // divide the fit's measurements by it. Re-baked on every injection, which
  // the native side triggers whenever the viewport or factor could change.
  float vf = GetViewportVirtScale();
  if (!(vf > 0.f))
    vf = 1.f;

  char js[1500];
  snprintf(js, sizeof(js),
    "(function(){"
    "var dw=%d,dh=%d,vf=%.4f;"
    "var de=document.documentElement,b=document.body;"
    "window.__iplugFit=function(){"
      "var iw=window.innerWidth/vf,ih=window.innerHeight/vf;"
      "if(!(iw>0&&ih>0&&dw>0&&dh>0))return;"
      // Undershoot by 2 CSS px per axis: innerWidth/Height are rounded UP from
      // the physical surface, the surface itself overshoots the parent by the
      // +1 in GetScaledRect, and the host may sit the parent 1-2px past the
      // frame. Scaling to the exact reported viewport parks the design's last
      // rows in that slop and they get shaved; a 2px inset keeps the true
      // bottom/right edge visibly inside in every host.
      "var s=Math.min((iw-2)/dw,(ih-2)/dh);"
      // center the letterboxed content: symmetric page-background margins look
      // intentional while the host window is mid-drag / not yet snapped
      "var tx=Math.max(0,(iw-dw*s)/2),ty=Math.max(0,(ih-dh*s)/2);"
      "de.style.width=dw+'px';de.style.height=dh+'px';de.style.overflow='hidden';de.style.margin='0';"
      "de.style.transform='translate('+tx+'px,'+ty+'px) scale('+s+')';de.style.transformOrigin='top left';"
      // margin:0 is load-bearing: the browser's default 8px body margin offsets
      // the body inside the (clipped) html box, pushing the design's last 8 CSS
      // px below the viewport — with position:relative set here, bottom-anchored
      // content sits in exactly those rows and gets shaved on every host.
      "b.style.width=dw+'px';b.style.height=dh+'px';b.style.position='relative';b.style.overflow='hidden';b.style.margin='0';"
    "};"
    "if(!window.__iplugFitBound){window.__iplugFitBound=true;"
      "window.addEventListener('resize',function(){window.__iplugFit&&window.__iplugFit();});"
      "var wd=function(){try{"
        "var q=matchMedia('(resolution: '+window.devicePixelRatio+'dppx)');"
        "var f=function(){window.__iplugFit&&window.__iplugFit();wd();};"
        "q.addEventListener?q.addEventListener('change',f,{once:true}):q.addListener(f);"
      "}catch(e){}};"
      "wd();}"
    "window.__iplugFit();"
    "setTimeout(window.__iplugFit,50);setTimeout(window.__iplugFit,200);"
    "setTimeout(window.__iplugFit,600);setTimeout(window.__iplugFit,1500);"
    // diagnostic: surfaced in the webview-init log via the fit_done trace
    "return JSON.stringify({iw:window.innerWidth,ih:window.innerHeight,vf:vf,"
    "dpr:window.devicePixelRatio,dw:dw,dh:dh,t:de.style.transform});"
    "})();",
    mDesignWidth, mDesignHeight, vf);
  EvaluateJavaScript(js, nullptr);
}

bool WebViewEditorDelegate::OnKeyDown(const IKeyPress& key)
{
  #ifdef OS_WIN
  if (key.VK == VK_SPACE)
  {
    // Forward spacebar to the DAW so transport (play/stop) still works while the
    // WebView is focused. CRITICAL: post to the host's TOP-LEVEL window (GA_ROOT),
    // never to mView. mView is the WebView's own parent HWND, so a synthetic key
    // sent there is routed back into the focused WebView, whose injected JS
    // re-reports it via SKPFUI -> OnKeyDown -> here = an infinite key-echo loop
    // that hangs the host UI thread (Windows-only; macOS bubbles via the responder
    // chain and cannot loop). GA_ROOT reaches the DAW's own WndProc instead, which
    // cannot re-enter the WebView. The short time-guard is belt-and-suspenders
    // against any host that reflects the synthetic key back to its focused child.
    const unsigned int nowMs = (unsigned int) GetTickCount();
    if (nowMs - mLastSpaceForwardMs >= 60)
    {
      mLastSpaceForwardMs = nowMs;
      HWND root = GetAncestor((HWND) mView, GA_ROOT);
      if (root)
      {
        // Forward as a synthetic WM_KEYDOWN/WM_KEYUP to the host's top-level
        // window so DAWs that bind transport (play/stop) to the space key —
        // via their accelerator table or main WndProc keyboard handler — still
        // toggle while the WebView is focused. lParam carries a real scan code
        // so input filters that reject lParam=0 as injected (e.g. FL Studio)
        // accept it. A WM_KEYDOWN posted to a specific HWND does not escape
        // that window, so it cannot leak system-wide.
        //
        // We deliberately do NOT also post WM_APPCOMMAND /
        // APPCOMMAND_MEDIA_PLAY_PAUSE here. It looks like a targeted message,
        // but DefWindowProc's default handling forwards an unhandled
        // WM_APPCOMMAND up the parent chain and ultimately to the shell as a
        // SYSTEM-WIDE media-key event — the exact path a hardware play/pause
        // key takes. Windows then routes it to the active media session, so
        // pressing space with the plugin focused would start/stop whatever the
        // user has playing (Spotify, etc.). Since DAW transport is bound to the
        // space key and not to the media-play-pause APPCOMMAND, that post never
        // helped transport and only produced the music-toggle bug. See
        // pluginmaker-ai/alexh — "spacebar toggles Spotify" fix.
        const LPARAM scan = (LPARAM) MapVirtualKey(VK_SPACE, MAPVK_VK_TO_VSC);
        PostMessage(root, WM_KEYDOWN, VK_SPACE, (scan << 16) | 0x00000001);
        PostMessage(root, WM_KEYUP,   VK_SPACE, (scan << 16) | 0xC0000001);
      }
    }
    return true;
  }
  #endif
  return false;
}

bool WebViewEditorDelegate::OnKeyUp(const IKeyPress& key)
{

  return true;
}