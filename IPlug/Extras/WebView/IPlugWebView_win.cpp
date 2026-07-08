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

#include "IPlugWebView.h"
#include "IPlugPaths.h"
#include <string>
#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <commctrl.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib") // SHCreateDirectoryExW

namespace
{
// WebView2 init diagnostic log — one file per host process, append mode,
// best-effort (never throws, never blocks the audio thread). Lives at
// %LOCALAPPDATA%\iPlug2\Logs\webview-init-<pid>.log. We open and close
// per write so multiple controllers in the same host don't fight a
// shared FILE handle and so a crash mid-init doesn't lose buffered
// lines. Format is one self-contained line per event so a customer can
// just zip the directory and send it.
//
// Why a custom log instead of OutputDebugString / iPlug2's DBGMSG:
// release-build customers don't have a debugger attached and DBGMSG is
// invisible to them. A real file on disk is the only thing they can
// screenshot or send.
void WebViewInitLog(const char* event, HRESULT hr, const char* detailFmt = nullptr, ...)
{
  WCHAR localAppData[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData)))
    return;

  WCHAR logDir[MAX_PATH] = {};
  swprintf_s(logDir, L"%s\\iPlug2\\Logs", localAppData);
  // Best-effort: ignore errors. If the dir can't be created we'll just
  // fail to write below — diagnostic log is never load-bearing.
  SHCreateDirectoryExW(nullptr, logDir, nullptr);

  WCHAR logPath[MAX_PATH] = {};
  swprintf_s(logPath, L"%s\\webview-init-%lu.log", logDir, GetCurrentProcessId());

  FILE* f = nullptr;
  if (_wfopen_s(&f, logPath, L"a") != 0 || !f) return;

  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [pid=%lu] event=%s hr=0x%08lX",
          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
          GetCurrentProcessId(), event ? event : "(null)", static_cast<unsigned long>(hr));

  if (detailFmt && *detailFmt)
  {
    fprintf(f, " detail=\"");
    va_list args;
    va_start(args, detailFmt);
    vfprintf(f, detailFmt, args);
    va_end(args);
    fprintf(f, "\"");
  }
  fputc('\n', f);
  fclose(f);
}
} // anonymous namespace

#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "wdlstring.h"

extern float GetScaleForHWND(HWND hWnd);

BEGIN_IPLUG_NAMESPACE

class IWebViewImpl
{
public:
  IWebViewImpl(IWebView* owner);
  ~IWebViewImpl();

  void* OpenWebView(void* pParent, float x, float y, float w, float h, float scale);
  void CloseWebView();
  void HideWebView(bool hide);
  void LoadHTML(const char* html);
  void LoadURL(const char* url);
  void LoadFile(const char* fileName, const char* bundleID);
  void ReloadPageContent();
  void EvaluateJavaScript(const char* scriptStr, IWebView::completionHandlerFunc func);
  void EnableScroll(bool enable);
  void EnableInteraction(bool enable);
  void SetWebViewBounds(float x, float y, float w, float h, float scale);
  void GetWebRoot(WDL_String& path) const { path.Set(mWebRoot.Get()); }
  void GetLocalDownloadPathForFile(const char* fileName, WDL_String& downloadPath);

private:
  RECT GetScaledRect(float x, float y, float w, float h, float scale)
  {
    RECT r;
    r.left = static_cast<LONG>(std::ceil(x * scale));
    r.top = static_cast<LONG>(std::ceil(y * scale));
    r.right = static_cast<LONG>(std::ceil((x + w) * scale)) + 1;
    r.bottom = static_cast<LONG>(std::ceil((y + h) * scale)) + 1;
    return r;
  }

  static LRESULT CALLBACK AspectRatioSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
  static LRESULT CALLBACK ParentWatchSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
  void InstallAspectRatioHook(int designWidth, int designHeight);
  void RemoveAspectRatioHook();
  bool NudgeClippedAncestorIntoView();
  bool ApplyWebViewBounds(); // returns true when the effective bounds/zoom actually changed
  void PinRasterizationScale();
  void SnapFrameToContent();
public:
  void SetMinSize(int minW, int minH) { mMinWidth = minW; mMinHeight = minH; }
private:

  IWebView* mIWebView;
  bool mOpaque;
  HWND mParentWnd = NULL;
  HWND mSubclassedHwnd = NULL;
  HWND mSubclassedParentWnd = NULL;
  bool mInSizeMove = false;
  // Deferred post-open re-measures still owed (see the open-settle timer armed
  // at controller-ready).
  int mOpenSettleShotsRemaining = 0;
  // Last bounds/zoom actually pushed to the controller — dedup guard so the
  // parent-watch and resize storms don't spam identical SetBoundsAndZoomFactor
  // calls (and identical log lines).
  RECT mLastPushedBounds = { -1, -1, -1, -1 };
  double mLastPushedZoom = -999.0;
  int mDesignWidth = 0;
  int mDesignHeight = 0;
  int mMinWidth = 0;
  int mMinHeight = 0;
  wil::com_ptr<ICoreWebView2Controller> mWebViewCtrlr;
  wil::com_ptr<ICoreWebView2> mCoreWebView;
  wil::com_ptr<ICoreWebView2Environment> mWebViewEnvironment;
  EventRegistrationToken mWebMessageReceivedToken;
  EventRegistrationToken mNavigationStartingToken;
  EventRegistrationToken mNavigationCompletedToken;
  EventRegistrationToken mNewWindowRequestedToken;
  EventRegistrationToken mDownloadStartingToken;
  EventRegistrationToken mBytesReceivedChangedToken;
  EventRegistrationToken mStateChangedToken;
  bool mShowOnLoad = true;
  WDL_String mWebRoot;
  WDL_String mVirtualHost; // per-instance WebView2 virtual host (see LoadFile); avoids same-host mapping collisions across instances
  RECT mWebViewBounds = {};
  // Last size request from SetWebViewBounds, replayed by the async
  // controller-creation callback so the initial bounds are computed against
  // the host window as it exists then, not as it was at request time.
  float mLastBoundsX = 0.f;
  float mLastBoundsY = 0.f;
  float mLastBoundsW = 0.f;
  float mLastBoundsH = 0.f;
  float mLastBoundsScale = 1.f;
  bool mHasLastBounds = false;
};

END_IPLUG_NAMESPACE

using namespace iplug;
using namespace Microsoft::WRL;

// Windows has no OS-level content-aspect-ratio lock like macOS's
// NSWindow setContentAspectRatio. To get the same behavior we subclass the
// host's top-level plugin window and intercept WM_SIZING, which fires during
// the user's live drag gesture and lets us clamp the rect in place before
// Windows (and the host DAW) ever see a non-proportional size.
static const UINT_PTR kAspectRatioSubclassId = 0x1AA5BEC7;
static const UINT_PTR kParentWatchSubclassId = 0x1AA5BEC8;
static const UINT_PTR kFrameSnapTimerId = 0x1AA5BEC9;
static const UINT_PTR kOpenSettleTimerId = 0x1AA5BECA;

LRESULT CALLBACK IWebViewImpl::AspectRatioSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
  if (msg == WM_NCDESTROY)
  {
    RemoveWindowSubclass(hWnd, &IWebViewImpl::AspectRatioSubclassProc, uIdSubclass);
    return DefSubclassProc(hWnd, msg, wParam, lParam);
  }

  if ((msg != WM_GETMINMAXINFO && msg != WM_SIZE && msg != WM_DPICHANGED &&
       msg != WM_ENTERSIZEMOVE && msg != WM_EXITSIZEMOVE && msg != WM_TIMER) || !dwRefData)
    return DefSubclassProc(hWnd, msg, wParam, lParam);

  IWebViewImpl* self = reinterpret_cast<IWebViewImpl*>(dwRefData);
  const int designW = self->mDesignWidth;
  const int designH = self->mDesignHeight;
  const int minW = (self->mMinWidth > 0) ? self->mMinWidth : 1;
  const int minH = (self->mMinHeight > 0) ? self->mMinHeight : 1;
  if (designW <= 0 || designH <= 0)
    return DefSubclassProc(hWnd, msg, wParam, lParam);

  // WM_GETMINMAXINFO is Windows's authoritative "what are your size bounds"
  // query — it's checked before any resize path, interactive or programmatic.
  // Setting ptMinTrackSize here makes Windows itself refuse to go smaller,
  // which closes the loophole where Ableton commits a sub-minimum size via
  // WM_SIZE directly (bypassing WM_SIZING).
  if (msg == WM_GETMINMAXINFO)
  {
    RECT windowRect, clientRect;
    GetWindowRect(hWnd, &windowRect);
    GetClientRect(hWnd, &clientRect);
    int ncW = (windowRect.right - windowRect.left) - clientRect.right;
    int ncH = (windowRect.bottom - windowRect.top) - clientRect.bottom;

    // Include the host's in-client chrome (see the WM_SIZING block) so the
    // minimum applies to the plugin area, not the whole frame client.
    if (self->mParentWnd && self->mParentWnd != hWnd)
    {
      RECT containerRect = {};
      if (GetClientRect(self->mParentWnd, &containerRect) && containerRect.right > 0 && containerRect.bottom > 0 &&
          clientRect.right >= containerRect.right && clientRect.bottom >= containerRect.bottom)
      {
        ncW += clientRect.right - containerRect.right;
        ncH += clientRect.bottom - containerRect.bottom;
      }
    }

    MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
    mmi->ptMinTrackSize.x = minW + ncW;
    mmi->ptMinTrackSize.y = minH + ncH;
    return 0;
  }

  // Track the interactive size/move loop, and after it ends (or after any
  // settled layout change, via the debounce timer) snap the host frame to hug
  // the plugin content: hosts like Studio One center the aspect-corrected view
  // inside whatever window the user dragged and paint the leftover as dead
  // black bands — they never shrink the window themselves.
  if (msg == WM_ENTERSIZEMOVE)
  {
    self->mInSizeMove = true;
    ::WebViewInitLog("AspectHook:ENTERSIZEMOVE", S_OK, nullptr);
    return DefSubclassProc(hWnd, msg, wParam, lParam);
  }
  if (msg == WM_EXITSIZEMOVE)
  {
    self->mInSizeMove = false;
    ::WebViewInitLog("AspectHook:EXITSIZEMOVE", S_OK, nullptr);
    SetTimer(hWnd, kFrameSnapTimerId, 60, nullptr);
    return DefSubclassProc(hWnd, msg, wParam, lParam);
  }
  if (msg == WM_TIMER)
  {
    if (wParam == kFrameSnapTimerId)
    {
      KillTimer(hWnd, kFrameSnapTimerId);
      ::WebViewInitLog("FrameSnap:timer_fire", S_OK, nullptr);
      self->SnapFrameToContent();
      return 0;
    }
    if (wParam == kOpenSettleTimerId)
    {
      KillTimer(hWnd, kOpenSettleTimerId);
      // Replay of the geometry events swallowed during async controller
      // creation (they are all gated on mWebViewCtrlr) — same body as the
      // WM_SIZE handler; the dedup guard makes stable-geometry shots free.
      bool changed = false;
      bool nudged = false;
      if (self->mHasLastBounds && self->mWebViewCtrlr)
      {
        // FL Studio (attached plugin windows): the first-open wrapper is
        // placed too low inside FL's client area — its bottom hangs clipped
        // below the frame and FL never repositions it (reopens spawn higher
        // and render fine, which is why only first opens letterboxed).
        // Verified live that a programmatic move up sticks: FL treats it
        // exactly like the user dragging the wrapper. Only child-ancestor
        // chains are nudged — top-level wrappers (Studio One / FSP) never
        // enter and keep today's clamp+letterbox behavior.
        nudged = self->NudgeClippedAncestorIntoView();
        changed = self->ApplyWebViewBounds();
        if (changed && self->mIWebView)
          self->mIWebView->OnWebViewViewportChanged();
      }
      ::WebViewInitLog("OpenSettle:timer_fire", S_OK, "changed=%d nudged=%d shotsLeft=%d",
                       changed ? 1 : 0, nudged ? 1 : 0, self->mOpenSettleShotsRemaining - 1);
      if (--self->mOpenSettleShotsRemaining > 0)
        SetTimer(hWnd, kOpenSettleTimerId, 700, nullptr);
      return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
  }

  // WM_DPICHANGED: the top-level frame crossed to a monitor with a different
  // DPI. Hosts like Studio One / Fender Studio Pro tell the plugin NOTHING here
  // (no onSize, no setContentScaleFactor) while re-laying-out their own chrome —
  // measured live: the chrome strip grows to chromeLogical*newScale and our
  // parent container is shifted down below it WITHOUT being resized, so its
  // bottom hangs outside the frame, clipped. Let the host's own handler run
  // first (DefSubclassProc), then self-correct: re-pin the rasterization scale,
  // re-apply + re-clamp the WebView bounds against the frame's new layout, and
  // have the delegate re-fit the content to the changed viewport.
  if (msg == WM_DPICHANGED)
  {
    const LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);
    if (self->mHasLastBounds && self->mWebViewCtrlr)
    {
      ::WebViewInitLog("AspectHook:WM_DPICHANGED", S_OK, "newDpi=%u", (unsigned)HIWORD(wParam));
      self->ApplyWebViewBounds();
      if (self->mIWebView)
        self->mIWebView->OnWebViewViewportChanged();
    }
    return res;
  }

  // WM_SIZE: the host committed a frame size. Studio One / Fender Studio Pro
  // clamp ONLY this top-level frame to the screen work area on a clean open
  // (delivered as a bare WM_SIZE, never WM_SIZING) while building our parent
  // container at the full requested size as a child of the clamped frame — so
  // the bottom of the WebView hangs below the visible frame edge, clipped, with
  // no scrollbar. GetClientRect(mParentWnd) can't see it (it stays full size);
  // only this frame is clamped. Re-apply so the visibility clamp inside
  // ApplyWebViewBounds re-reads the frame's real client region, and re-fit the
  // content. (Idempotent — on ordinary host-driven resizes the host's onSize
  // storm re-applies with identical values anyway.)
  if (msg == WM_SIZE)
  {
    if (self->mHasLastBounds && self->mWebViewCtrlr)
    {
      self->ApplyWebViewBounds();
      if (self->mIWebView)
        self->mIWebView->OnWebViewViewportChanged();
      // debounce a frame snap for hosts that never run the standard size-move
      // loop (no WM_ENTER/EXITSIZEMOVE) — reset on every size event, fires
      // once the layout settles
      if (!self->mInSizeMove)
        SetTimer(hWnd, kFrameSnapTimerId, 80, nullptr);
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
  }

  // NOTE: we deliberately do NOT intercept WM_SIZING to aspect-lock the drag
  // rectangle anymore. Measured live on Fender Studio Pro: FSP re-applies its
  // own mouse-driven width right after our correction, so the two fight and the
  // window visibly oscillates (~192px) throughout the drag. Aspect is now owned
  // by the injected viewport-fit (letterboxes the content, centered, during the
  // drag) and by the post-release frame snap (trims the host window to hug the
  // content). No mid-drag window correction = no fight = no flicker.
  return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// The host can re-layout our parent container SILENTLY — measured on Fender
// Studio Pro: after an interactive resize ends (and on some DPI transitions)
// the container is moved/cropped within the host frame with NO WM_SIZE on the
// frame and NO onSize to the plugin, leaving the WebView's bottom outside the
// visible area. Watching the parent itself closes that blind spot: any position
// or size change re-runs ApplyWebViewBounds (which re-measures the visible
// intersection) and, only when the effective bounds really changed, asks the
// delegate to re-fit the content. The dedup guard in ApplyWebViewBounds makes
// the storm-case (WM_WINDOWPOSCHANGED spam during drags) a cheap no-op.
LRESULT CALLBACK IWebViewImpl::ParentWatchSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
  if (msg == WM_NCDESTROY)
  {
    RemoveWindowSubclass(hWnd, &IWebViewImpl::ParentWatchSubclassProc, uIdSubclass);
    return DefSubclassProc(hWnd, msg, wParam, lParam);
  }

  if ((msg != WM_WINDOWPOSCHANGED && msg != WM_SIZE && msg != WM_MOVE) || !dwRefData)
    return DefSubclassProc(hWnd, msg, wParam, lParam);

  const LRESULT res = DefSubclassProc(hWnd, msg, wParam, lParam);

  IWebViewImpl* self = reinterpret_cast<IWebViewImpl*>(dwRefData);
  if (self->mHasLastBounds && self->mWebViewCtrlr)
  {
    if (self->ApplyWebViewBounds() && self->mIWebView)
      self->mIWebView->OnWebViewViewportChanged();
    // the host re-laid-out our container — debounce a frame snap so dead
    // letterbox bands (host centers a smaller view, keeps its window) collapse
    if (!self->mInSizeMove && self->mSubclassedHwnd)
      SetTimer(self->mSubclassedHwnd, kFrameSnapTimerId, 80, nullptr);
  }
  return res;
}

void IWebViewImpl::InstallAspectRatioHook(int designWidth, int designHeight)
{
  if (mSubclassedHwnd || !mParentWnd || designWidth <= 0 || designHeight <= 0)
    return;

  mDesignWidth = designWidth;
  mDesignHeight = designHeight;

  // Walk up to the top-level window — WM_SIZING only fires on the outermost
  // window of the resize drag, which is the host's plugin frame window.
  HWND topLevel = GetAncestor(mParentWnd, GA_ROOT);
  if (!topLevel)
    return;

  if (SetWindowSubclass(topLevel, &IWebViewImpl::AspectRatioSubclassProc, kAspectRatioSubclassId, reinterpret_cast<DWORD_PTR>(this)))
  {
    mSubclassedHwnd = topLevel;
  }

  // Watch the parent container itself for silent host re-layouts (see
  // ParentWatchSubclassProc). Skip when the parent IS the top-level window
  // (standalone app) — the frame subclass already covers it.
  if (mParentWnd != topLevel &&
      SetWindowSubclass(mParentWnd, &IWebViewImpl::ParentWatchSubclassProc, kParentWatchSubclassId, reinterpret_cast<DWORD_PTR>(this)))
  {
    mSubclassedParentWnd = mParentWnd;
  }
}

void IWebViewImpl::RemoveAspectRatioHook()
{
  if (mSubclassedHwnd)
  {
    KillTimer(mSubclassedHwnd, kFrameSnapTimerId);
    KillTimer(mSubclassedHwnd, kOpenSettleTimerId);
    RemoveWindowSubclass(mSubclassedHwnd, &IWebViewImpl::AspectRatioSubclassProc, kAspectRatioSubclassId);
    mSubclassedHwnd = NULL;
  }
  if (mSubclassedParentWnd)
  {
    RemoveWindowSubclass(mSubclassedParentWnd, &IWebViewImpl::ParentWatchSubclassProc, kParentWatchSubclassId);
    mSubclassedParentWnd = NULL;
  }
}

IWebViewImpl::IWebViewImpl(IWebView* owner)
  : mIWebView(owner)
{
}

IWebViewImpl::~IWebViewImpl()
{
  CloseWebView();
}

void* IWebViewImpl::OpenWebView(void* pParent, float, float, float w, float h, float)
{
  mParentWnd = (HWND)pParent;

  // Install the Win32 aspect-ratio hook now that we know the parent HWND.
  // w/h here are the design dimensions passed by IPlugWebViewEditorDelegate
  // on first open (they equal GetEditorWidth/Height).
  InstallAspectRatioHook(static_cast<int>(w), static_cast<int>(h));

  WDL_String cachePath;
  WebViewCachePath(cachePath);
  int bufSize = UTF8ToUTF16Len(cachePath.Get());
  std::vector<WCHAR> cachePathWide(bufSize);
  UTF8ToUTF16(cachePathWide.data(), cachePath.Get(), IPLUG_WIN_MAX_WIDE_PATH);

  // Pre-create the UDF parent path. WebView2 will create it itself on
  // most configurations, but on machines with restrictive AV / EDR or
  // weird ACL inheritance the silent failure surfaces upstream as a
  // blank WebView. Doing it ourselves with a documented Windows API
  // gives us a logged GetLastError if the create step is where things
  // fall over. SHCreateDirectoryExW handles intermediate path creation
  // and returns ERROR_ALREADY_EXISTS as a non-error.
  {
    int rc = SHCreateDirectoryExW(nullptr, cachePathWide.data(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
    {
      ::WebViewInitLog("OpenWebView:SHCreateDirectoryEx_failed", HRESULT_FROM_WIN32(rc),
                       "cachePath='%s' GetLastError=%lu", cachePath.Get(), GetLastError());
    }
  }

  auto options = Make<CoreWebView2EnvironmentOptions>();
  options->put_AllowSingleSignOnUsingOSPrimaryAccount(FALSE);
  options->put_ExclusiveUserDataFolderAccess(FALSE);
  // options->put_Language(m_language.c_str());
  options->put_IsCustomCrashReportingEnabled(FALSE);
  options->put_AdditionalBrowserArguments(L"--disable-gpu");

  ::WebViewInitLog("OpenWebView:start", S_OK,
                   "cachePath='%s' parentHwnd=%p", cachePath.Get(), (void*)mParentWnd);

  CreateCoreWebView2EnvironmentWithOptions(
    nullptr, cachePathWide.data(), options.Get(),
    Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](
                                                                           HRESULT result,
                                                                           ICoreWebView2Environment* env) -> HRESULT {
      // Null-check env BEFORE dereferencing. If env-creation actually
      // failed (env is null), the previous code path-faulted on the
      // CreateCoreWebView2Controller call below — a latent crash bug.
      // Surface the HRESULT to the log either way so we know whether
      // env creation is where things fall over on a customer machine.
      if (FAILED(result) || env == nullptr)
      {
        ::WebViewInitLog("env:create_failed", result, "env=%p", (void*)env);
        return result;
      }
      ::WebViewInitLog("env:create_ok", result, nullptr);

      mWebViewEnvironment = env;

      mWebViewEnvironment->CreateCoreWebView2Controller(
        mParentWnd,
          Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([&](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (FAILED(result) || controller == nullptr)
            {
              ::WebViewInitLog("controller:create_failed", result, "controller=%p", (void*)controller);
              return result;
            }
            ::WebViewInitLog("controller:create_ok", result, nullptr);

            mWebViewCtrlr = controller;
            mWebViewCtrlr->get_CoreWebView2(&mCoreWebView);

            // A fresh controller starts at 0x0 and has never been pushed to,
            // but the push-dedup cache (mLastPushedBounds/mLastPushedZoom)
            // survives CloseWebView -> OpenWebView on the same editor
            // instance. When a reopen recomputes exactly the bounds of the
            // previous session's last push (the common case: same wrapper
            // size), ApplyWebViewBounds dedup-skipped the push and the new
            // controller stayed 0x0 — a fully black editor until the next
            // real resize broke the equality (measured in FL Studio 2025:
            // warm reopens black 3 of 6, un-bricked by manually resizing the
            // plugin window). Reset the cache at controller birth so the
            // first apply after (re)creation always pushes.
            mLastPushedBounds = { -1, -1, -1, -1 };
            mLastPushedZoom = -999.0;

            if (mCoreWebView == nullptr)
            {
              ::WebViewInitLog("controller:get_CoreWebView2_null", E_POINTER, nullptr);
              return S_OK;
            }

            mWebViewCtrlr->put_IsVisible(mShowOnLoad);

            const auto enableDevTools = true; // TEMP: force devtools for debugging

            ICoreWebView2Settings* Settings;
            mCoreWebView->get_Settings(&Settings);
            Settings->put_IsScriptEnabled(TRUE);
            Settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            Settings->put_IsWebMessageEnabled(TRUE);
            Settings->put_AreDefaultContextMenusEnabled(enableDevTools);
            Settings->put_AreDevToolsEnabled(enableDevTools);

            // this script adds a function IPlugSendMsg that is used to communicate from the WebView to the C++ side
            mCoreWebView->AddScriptToExecuteOnDocumentCreated(
              L"function IPlugSendMsg(m) {window.chrome.webview.postMessage(m)};",
              Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([this](HRESULT error,
                                                                                                PCWSTR id) -> HRESULT {
                return S_OK;
              }).Get());

            // Forward keydown / keyup from the WebView to the C++ side. The
            // editable check is intentionally aggressive: any focused element
            // that could plausibly accept typed text (including shadow-DOM
            // hosts, contenteditable regions, ARIA role=textbox/searchbox/
            // combobox/spinbutton, and IME composition) is treated as
            // "typing in the plugin UI" and we let the WebView default
            // handle the key — so space types a space character. For
            // anything else, we forward SPACE to the host via SKPFUI so
            // OnKeyDown can post WM_KEYDOWN to GA_ROOT for DAW transport.
            //
            // Capture-phase listener (3rd arg true) so plugin UI frameworks
            // (React, Vue, lit-html) can't stopPropagation() and prevent us
            // from seeing the key. Only SPACE is forwarded to the host
            // today — other keys are left for the WebView to handle.
            const wchar_t* kForwardKeysScript =
              L"(function(){"
              L"function isEditableTarget(e){"
              L"  if(e.isComposing||e.keyCode===229) return true;"
              L"  var path=(typeof e.composedPath==='function')?e.composedPath():[];"
              L"  var candidates=path.length?path:[document.activeElement];"
              L"  for(var i=0;i<candidates.length;i++){"
              L"    var n=candidates[i];"
              L"    if(!n||n===document||n===window||!n.tagName) continue;"
              L"    var tag=n.tagName.toUpperCase();"
              L"    if(tag==='INPUT'||tag==='TEXTAREA'||tag==='SELECT') return true;"
              L"    if(n.isContentEditable===true) return true;"
              L"    if(n.getAttribute){"
              L"      var role=n.getAttribute('role');"
              L"      if(role==='textbox'||role==='searchbox'||role==='combobox'||role==='spinbutton') return true;"
              L"    }"
              L"  }"
              L"  return false;"
              L"}"
              L"document.addEventListener('keydown',function(e){"
              L"  if(e.keyCode!==32) return;"
              L"  if(isEditableTarget(e)) return;"
              L"  e.preventDefault();"
              L"  IPlugSendMsg({msg:'SKPFUI',keyCode:e.keyCode,utf8:e.key,S:e.shiftKey,C:e.ctrlKey,A:e.altKey,isUp:false});"
              L"},true);"
              L"document.addEventListener('keyup',function(e){"
              L"  if(e.keyCode!==32) return;"
              L"  if(isEditableTarget(e)) return;"
              L"  e.preventDefault();"
              L"  IPlugSendMsg({msg:'SKPFUI',keyCode:e.keyCode,utf8:e.key,S:e.shiftKey,C:e.ctrlKey,A:e.altKey,isUp:true});"
              L"},true);"
              L"})();";

            mCoreWebView->AddScriptToExecuteOnDocumentCreated(
              kForwardKeysScript,
              Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([this](HRESULT error, PCWSTR id) -> HRESULT { return S_OK; }).Get());

            mCoreWebView->add_WebMessageReceived(
              Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](
                                                                      ICoreWebView2* sender,
                                                                      ICoreWebView2WebMessageReceivedEventArgs* args) {
                wil::unique_cotaskmem_string jsonString;
                args->get_WebMessageAsJson(&jsonString);
                std::wstring jsonWString = jsonString.get();
                WDL_String cStr;
                UTF16ToUTF8(cStr, jsonWString.c_str());
                mIWebView->OnMessageFromWebView(cStr.Get());
                return S_OK;
              }).Get(),
              &mWebMessageReceivedToken);


            mCoreWebView->add_NavigationStarting(
              Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                  
                  wil::unique_cotaskmem_string uri;
                  args->get_Uri(&uri);
                  std::wstring uriUTF16 = uri.get();
                  WDL_String uriUTF8;
                  UTF16ToUTF8(uriUTF8, uriUTF16.c_str());
                  
                  if (mIWebView->OnCanNavigateToURL(uriUTF8.Get()) == false)
                  {
                    args->put_Cancel(TRUE);
                  }

                  return S_OK;
                }).Get(),
              &mNavigationStartingToken);

            mCoreWebView->add_NavigationCompleted(
              Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                  BOOL success = FALSE;
                  args->get_IsSuccess(&success);
                  if (success)
                  {
                    ::WebViewInitLog("NavigationCompleted:ok", S_OK, nullptr);
                    mIWebView->OnWebContentLoaded();
                  }
                  else
                  {
                    // The smoking gun for the virtual-host no-op: if the mapping
                    // silently didn't attach, the Navigate escaped to real DNS and
                    // lands here with HostNameResolved / ConnectionAborted -- the
                    // failure that previously showed the user the Edge error page.
                    COREWEBVIEW2_WEB_ERROR_STATUS webErrorStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_WebErrorStatus(&webErrorStatus);
                    ::WebViewInitLog("NavigationCompleted:failed", E_FAIL,
                                     "webErrorStatus=%d (COREWEBVIEW2_WEB_ERROR_STATUS)", (int) webErrorStatus);
                  }
                  return S_OK;
                })
                .Get(),
              &mNavigationCompletedToken);

              mCoreWebView->add_NewWindowRequested(
              Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                  [this](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args)
                    -> HRESULT 
              {
                wil::com_ptr<ICoreWebView2NewWindowRequestedEventArgs2> args2;

                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))))
                {
                  DWORD inetStatus = 0;
                  if (InternetGetConnectedState(&inetStatus, 0))
                  {
                    wil::unique_cotaskmem_string uri;

                    args2->get_Uri(&uri);

                    if (ShellExecuteW(mParentWnd, L"open", uri.get(), 0, 0, SW_SHOWNORMAL) > HINSTANCE(32))
                    {
                      args->put_Handled(true);
                    }
                  }
                }
                return S_OK;
              }).Get(),
                &mNewWindowRequestedToken);

              auto webView2_4 = mCoreWebView.try_query<ICoreWebView2_4>();
              if (webView2_4)
              {
                webView2_4->add_DownloadStarting(
                  Callback<ICoreWebView2DownloadStartingEventHandler>(
                  [this](
                    ICoreWebView2* sender,
                    ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {

                    // Hide the default download dialog.
                    args->put_Handled(TRUE);

                    wil::com_ptr<ICoreWebView2DownloadOperation> download;
                    args->get_DownloadOperation(&download);

                    INT64 totalBytesToReceive = 0;
                    download->get_TotalBytesToReceive(&totalBytesToReceive);

                    // validate MIME type
                    wil::unique_cotaskmem_string mimeType;
                    download->get_MimeType(&mimeType);
                    std::wstring mimeTypeUTF16 = mimeType.get();
                    WDL_String mimeTypeUTF8;
                    UTF16ToUTF8(mimeTypeUTF8, mimeTypeUTF16.c_str());
                    if (!mIWebView->OnCanDownloadMIMEType(mimeTypeUTF8.Get()))
                    {
                      args->put_Cancel(TRUE);
                    }

                    wil::unique_cotaskmem_string contentDisposition;
                    download->get_ContentDisposition(&contentDisposition);

                    // Modify download path
                    wil::unique_cotaskmem_string resultFilePath;
                    args->get_ResultFilePath(&resultFilePath);

                    std::wstring initialPathUTF16 = resultFilePath.get();
                    WDL_String initialPathUTF8, downloadPathUTF8;
                    UTF16ToUTF8(initialPathUTF8, initialPathUTF16.c_str());
                    mIWebView->OnGetLocalDownloadPathForFile(initialPathUTF8.Get(), downloadPathUTF8);

                    int bufSize = UTF8ToUTF16Len(downloadPathUTF8.Get());
                    std::vector<WCHAR> downloadPathWide(bufSize);
                    UTF8ToUTF16(downloadPathWide.data(), downloadPathUTF8.Get(), bufSize);

                    args->put_ResultFilePath(downloadPathWide.data());
                    
                    download->add_BytesReceivedChanged(Callback<ICoreWebView2BytesReceivedChangedEventHandler>([this](ICoreWebView2DownloadOperation* download, IUnknown* args) -> HRESULT {
                                                         INT64 bytesReceived, totalNumBytes;
                                                         download->get_BytesReceived(&bytesReceived);
                                                         download->get_TotalBytesToReceive(&totalNumBytes);
                                                         mIWebView->OnReceivedData(bytesReceived, totalNumBytes);
                          return S_OK;
                        }).Get(),
                        &mBytesReceivedChangedToken);

                        download->add_StateChanged(Callback<ICoreWebView2StateChangedEventHandler>([this](ICoreWebView2DownloadOperation* download, IUnknown* args) -> HRESULT {
                                                               COREWEBVIEW2_DOWNLOAD_STATE downloadState;
                                                               download->get_State(&downloadState);

                                                               auto onDownloadEnded = [&](ICoreWebView2DownloadOperation* download, bool success) {
                                                                 download->remove_BytesReceivedChanged(mBytesReceivedChangedToken);
                                                                 download->remove_StateChanged(mStateChangedToken);
                                                                 wil::unique_cotaskmem_string resultFilePath;
                                                                 download->get_ResultFilePath(&resultFilePath);
                                                                 std::wstring downloadPathUTF16 = resultFilePath.get();
                                                                 WDL_String downloadPathUTF8;
                                                                 UTF16ToUTF8(downloadPathUTF8, downloadPathUTF16.c_str());

                                                                 if (success) {
                                                                  mIWebView->OnDownloadedFile(downloadPathUTF8.Get());
                                                                 }
                                                                 else {
                                                                  mIWebView->OnFailedToDownloadFile(downloadPathUTF8.Get());
                                                                 }
                                                               };

                                                               switch (downloadState)
                                                               {
                                                               case COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS:
                                                               // TODO
                                                                 break;
                                                               case COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED:
                                                                 onDownloadEnded(download, false);
                                                                 break;
                                                               case COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED:
                                                                 onDownloadEnded(download, true);
                                                                 break;
                                                               }
                                                               return S_OK;
                                                             }).Get(),
                                                             &mStateChangedToken);

                    return S_OK;
                  })
                  .Get(),
              &mDownloadStartingToken);
            }

            if (!mOpaque)
            {
              wil::com_ptr<ICoreWebView2Controller2> controller2 = mWebViewCtrlr.query<ICoreWebView2Controller2>();
              COREWEBVIEW2_COLOR color;
              memset(&color, 0, sizeof(COREWEBVIEW2_COLOR));
              controller2->put_DefaultBackgroundColor(color);
            }

            // Own the rasterization scale from the first frame. By default
            // WebView2 auto-detects monitor scale changes and re-rasterizes at
            // its own pace — racing our bounds math and the injected CSS fit
            // (measured: the fit read the viewport, then the auto-raster kicked
            // in and shrank it, leaving the content ~monitorScale too large and
            // the bottom clipped). Pin it to the parent window's DPI so the
            // page's devicePixelRatio is deterministic; re-pinned on every
            // ApplyWebViewBounds and on WM_DPICHANGED.
            PinRasterizationScale();

            // Replay the latest size request now that the controller exists.
            // The host may have (re)sized the parent window during the async
            // controller creation, so recompute against the live client rect
            // instead of pushing the possibly-stale mWebViewBounds.
            if (mHasLastBounds)
            {
              ApplyWebViewBounds();
            }
            else
            {
              // No host onSize yet. Some hosts (e.g. Studio One / Fender Studio
              // Pro with this plugin) never send an initial onSize — they wait
              // for the first resize/DPI event. Seed the controller bounds from
              // the parent's actual client rect so the UI is visible on OPEN
              // instead of a black rectangle until the user resizes. The proper
              // sentinel-aware sizing lands as soon as onSize does fire.
              RECT seed = {};
              bool haveSeed = GetClientRect(mParentWnd, &seed) && seed.right > 0 && seed.bottom > 0;
              if (!haveSeed && mDesignWidth > 0 && mDesignHeight > 0)
              {
                float dpiScale = GetScaleForHWND(mParentWnd);
                if (dpiScale <= 0.f) dpiScale = 1.f;
                seed = GetScaledRect(0.f, 0.f, static_cast<float>(mDesignWidth), static_cast<float>(mDesignHeight), dpiScale);
                haveSeed = true;
              }
              mWebViewCtrlr->put_Bounds(haveSeed ? seed : mWebViewBounds);
              mWebViewBounds = haveSeed ? seed : mWebViewBounds;
              mWebViewCtrlr->put_IsVisible(TRUE);
              ::WebViewInitLog("controller:seed_bounds", S_OK, "rect=%ldx%ld", seed.right, seed.bottom);
            }

            // First-open geometry storms happen while the controller is still
            // being created — FL Studio builds its wrapper as a small stub and
            // grows it to final size ~100ms after attach, and every re-measure
            // path (WM_SIZE, parent-watch) is gated on mWebViewCtrlr, so those
            // events are silently swallowed. The one measurement that does run
            // then sees a mid-grow rect, the visibility clamp letterboxes to
            // it, and nothing ever corrects it (measured: first open clamped
            // to 1200x667/603/571, reopen of the same window fine at
            // 1200x800). Arm a deferred settle re-measure now that the
            // controller exists: two shots, dedup-guarded no-ops when the
            // geometry is already stable.
            if (mSubclassedHwnd)
            {
              mOpenSettleShotsRemaining = 2;
              SetTimer(mSubclassedHwnd, kOpenSettleTimerId, 300, nullptr);
            }

#if defined APP_API
            // Standalone (APP) only: move keyboard focus into the WebView2 so
            // the web UI's document-level keydown/keyup listeners actually fire
            // — most importantly the computer-keyboard → MIDI handler that plays
            // notes (A/S/D… white, W/E/T… black, Z/X octave). The WebView2 gets
            // mouse input without focus (so clicking the on-screen keys works),
            // but never sees keydown until something moves focus into it, and
            // nothing in the APP host does. In a DAW the host manages focus, so
            // this is gated to the APP target to avoid stealing focus there.
            // See also IPlugAPP_main.cpp's message pump, which stops
            // IsDialogMessage from swallowing these keys once focus is here.
            if (mShowOnLoad)
              mWebViewCtrlr->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
#endif

            ::WebViewInitLog("controller:OnWebViewReady_fire", S_OK, nullptr);
            mIWebView->OnWebViewReady();
            return S_OK;
          })
          .Get());

      return S_OK;
    }).Get());

  return mParentWnd;
}

void IWebViewImpl::CloseWebView()
{
  RemoveAspectRatioHook();

  if (mWebViewCtrlr.get() != nullptr)
  {
    // Clear our per-instance virtual-host mapping so registrations don't leak
    // across editor open/close cycles within the shared browser process.
    if (mCoreWebView && mVirtualHost.GetLength() > 0)
    {
      if (auto webView3 = mCoreWebView.try_query<ICoreWebView2_3>())
      {
        int vhostLen = UTF8ToUTF16Len(mVirtualHost.Get());
        std::vector<WCHAR> vhostWide(vhostLen);
        UTF8ToUTF16(vhostWide.data(), mVirtualHost.Get(), vhostLen);
        webView3->ClearVirtualHostNameToFolderMapping(vhostWide.data());
      }
    }

    mWebViewCtrlr->Close();
    mWebViewCtrlr = nullptr;
    mCoreWebView = nullptr;
    mWebViewEnvironment = nullptr;
  }
}

void IWebViewImpl::HideWebView(bool hide)
{
  if (mWebViewCtrlr.get() != nullptr)
  {
    mWebViewCtrlr->put_IsVisible(!hide);
  }
  else
  {
    // the controller is set asynchonously, so we store the state 
    // to apply it when the controller is created
    mShowOnLoad = !hide;
  }
}

void IWebViewImpl::LoadHTML(const char* html)
{
  if (mCoreWebView)
  {
    int bufSize = UTF8ToUTF16Len(html);
    std::vector<WCHAR> htmlWide(bufSize);
    UTF8ToUTF16(htmlWide.data(), html, bufSize);
    mCoreWebView->NavigateToString(htmlWide.data());
  }
}

void IWebViewImpl::LoadURL(const char* url)
{
  if (mCoreWebView)
  {
    int bufSize = UTF8ToUTF16Len(url);
    std::vector<WCHAR> urlWide(bufSize);
    UTF8ToUTF16(urlWide.data(), url, bufSize);
    mCoreWebView->Navigate(urlWide.data());
  }
}

void IWebViewImpl::LoadFile(const char* fileName, const char* bundleID)
{
  // Diagnose-first instrumentation. If LoadFile fires while mCoreWebView
  // is still null (race against the async controller-creation callback),
  // the virtual-host mapping for iplug.example is never registered and
  // the subsequent Navigate falls through to real DNS (the
  // ERR_NAME_NOT_RESOLVED symptom). Surface that explicitly so the next
  // stuck-customer log tells us if this is the race we suspect.
  if (!mCoreWebView)
  {
    ::WebViewInitLog("LoadFile:skipped", S_FALSE,
                     "mCoreWebView=null at LoadFile call — race with controller-creation callback. fileName='%s'",
                     fileName ? fileName : "(null)");
    return;
  }

  // Per-instance virtual host. Every iPlug2 WebView plugin used to map the
  // SAME host ("iplug.example"); with ExclusiveUserDataFolderAccess(FALSE)
  // multiple WebViews share one browser process, and Microsoft documents
  // inconsistent same-host mappings across WebViews as undefined behavior.
  // In practice SetVirtualHostNameToFolderMapping then returns S_OK but the
  // resource filter silently never attaches, so Navigate escapes to real DNS
  // (the ERR_NAME_NOT_RESOLVED symptom). A host unique to this instance can
  // never collide, no matter how many other WebView plugins are loaded.
  if (mVirtualHost.GetLength() == 0)
    mVirtualHost.SetFormatted(64, "iplug-%p.example", (void*) this);

  WDL_String webFolder{fileName};
  webFolder.remove_filepart();
  mWebRoot.Set(webFolder.Get());

  bool mappingOk = false;

  wil::com_ptr<ICoreWebView2_3> webView3 = mCoreWebView.try_query<ICoreWebView2_3>();
  if (!webView3)
  {
    // ICoreWebView2_3 has shipped in every WebView2 Runtime since ~2021; a
    // missing interface means the runtime is severely out of date, which
    // would also affect any other modern WebView2 plugin on the machine.
    ::WebViewInitLog("LoadFile:no_ICoreWebView2_3", E_NOINTERFACE,
                     "try_query<ICoreWebView2_3> returned null -- WebView2 Runtime too old?");
  }
  else
  {
    int folderLen = UTF8ToUTF16Len(webFolder.Get());
    std::vector<WCHAR> webFolderWide(folderLen);
    UTF8ToUTF16(webFolderWide.data(), webFolder.Get(), folderLen);

    int vhostLen = UTF8ToUTF16Len(mVirtualHost.Get());
    std::vector<WCHAR> vhostWide(vhostLen);
    UTF8ToUTF16(vhostWide.data(), mVirtualHost.Get(), vhostLen);

    HRESULT mapHr = webView3->SetVirtualHostNameToFolderMapping(
      vhostWide.data(), webFolderWide.data(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(mapHr))
    {
      ::WebViewInitLog("LoadFile:SetVirtualHostNameToFolderMapping_failed", mapHr,
                       "vhost='%s' webFolder='%s'", mVirtualHost.Get(), webFolder.Get());
    }
    else
    {
      // Log the success path too, so a clean mapping is positively recorded
      // instead of being the (previously ambiguous) absence of a failure line.
      mappingOk = true;
      ::WebViewInitLog("LoadFile:mapping_ok", S_OK,
                       "vhost='%s' webFolder='%s'", mVirtualHost.Get(), webFolder.Get());
    }
  }

  // Fail closed. If the mapping was not established, navigating to the vhost
  // URL would escape to real DNS and show the user the Edge "can't reach this
  // page" error. Show an inline error page instead -- no network, no asset
  // refs -- naming the diagnostic log so a stuck customer can send it.
  if (!mappingOk)
  {
    ::WebViewInitLog("LoadFile:fail_closed_inline_error", E_FAIL,
                     "mapping not established; showing inline error instead of navigating");
    LoadHTML(
      "<!doctype html><meta charset=\"utf-8\">"
      "<style>html,body{height:100%;margin:0}"
      "body{background:#1e1e1e;color:#e2e8f0;font:13px/1.6 system-ui,sans-serif;"
      "display:flex;align-items:center;justify-content:center}"
      "div{max-width:34em;padding:2em;text-align:center}"
      "code{color:#9ca3af;font-size:12px}</style>"
      "<div><h2>Plugin UI could not start</h2>"
      "<p>The embedded view could not be initialized on this machine.</p>"
      "<p>Diagnostic log:<br><code>%LOCALAPPDATA%\\iPlug2\\Logs\\webview-init-&lt;pid&gt;.log</code></p>"
      "</div>");
    return;
  }

  // Mapping established -- safe to navigate to the per-instance virtual host.
  WDL_String baseName{fileName};
  WDL_String fullStr;
  fullStr.SetFormatted(2048, "https://%s/%s", mVirtualHost.Get(), baseName.get_filepart());
  int urlLen = UTF8ToUTF16Len(fullStr.Get());
  std::vector<WCHAR> fileUrlWide(urlLen);
  UTF8ToUTF16(fileUrlWide.data(), fullStr.Get(), urlLen);
  mCoreWebView->Navigate(fileUrlWide.data());
}


void IWebViewImpl::ReloadPageContent()
{
  if (mCoreWebView)
  {
    mCoreWebView->Reload();
  }
}

void IWebViewImpl::EvaluateJavaScript(const char* scriptStr, IWebView::completionHandlerFunc func)
{
  // The viewport-fit script is load-bearing for DPI correctness — trace its
  // submission, silent drops (no webview yet), and completion result so a
  // missing fit is visible in the webview-init log instead of a mystery.
  const bool isFitScript = scriptStr && strstr(scriptStr, "__iplugFit") != nullptr;

  if (mCoreWebView)
  {
    if (isFitScript)
      ::WebViewInitLog("EvaluateJavaScript:fit_submit", S_OK, "len=%d", (int)strlen(scriptStr));

    int bufSize = UTF8ToUTF16Len(scriptStr);
    std::vector<WCHAR> scriptWide(bufSize);
    UTF8ToUTF16(scriptWide.data(), scriptStr, bufSize);

    mCoreWebView->ExecuteScript(
      scriptWide.data(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([func, isFitScript](HRESULT errorCode,
                                                                              LPCWSTR resultObjectAsJson) -> HRESULT {
                    if (isFitScript)
                      ::WebViewInitLog("EvaluateJavaScript:fit_done", errorCode, "result=%S",
                                       resultObjectAsJson ? resultObjectAsJson : L"(null)");
                    if (func && resultObjectAsJson)
                    {
                      WDL_String str;
                      UTF16ToUTF8(str, resultObjectAsJson);
                      func(str.Get());
                    }
                    return S_OK;
                  }).Get());
  }
  else if (isFitScript)
  {
    ::WebViewInitLog("EvaluateJavaScript:fit_dropped", E_FAIL, "no CoreWebView yet");
  }
}

void IWebViewImpl::EnableScroll(bool enable)
{
  /* NO-OP */
}

void IWebViewImpl::EnableInteraction(bool enable)
{
  /* NO-OP */
}

void IWebViewImpl::SetWebViewBounds(float x, float y, float w, float h, float scale)
{
  mLastBoundsX = x;
  mLastBoundsY = y;
  mLastBoundsW = w;
  mLastBoundsH = h;
  mLastBoundsScale = scale;
  mHasLastBounds = true;

  ApplyWebViewBounds();
}

bool IWebViewImpl::ApplyWebViewBounds()
{
  const float x = mLastBoundsX;
  const float y = mLastBoundsY;
  const float w = mLastBoundsW;
  const float h = mLastBoundsH;
  const float scale = mLastBoundsScale;

  float dpiScale = GetScaleForHWND(mParentWnd);
  if (dpiScale <= 0.f) dpiScale = 1.f;
  mWebViewBounds = GetScaledRect(x, y, w, h, dpiScale);

  if (!mWebViewCtrlr)
    return false;

  // Keep the rasterization scale in lockstep with the parent window's DPI
  // (idempotent; see the comment at the controller-ready pin).
  PinRasterizationScale();

  // scale == -1: FL Studio — zoom = 1/dpiScale, normal DPI bounds
  // scale == -2: Cubase — zoom = 1.0, skip DPI bounds (host sends physical pixels)
  // scale >= 0: Ableton/default — pass through, then reconcile below
  float zoom = 1.f;
  if (scale == -1.f)
  {
    zoom = 1.f / dpiScale;
  }
  else if (scale == -2.f)
  {
    mWebViewBounds = GetScaledRect(x, y, w, h, 1.f);
    zoom = 1.f;
  }
  else
  {
    zoom = scale;

    // Reconcile against the parent's actual client rect. Ableton 12.4's
    // "Auto-Scale Plug-In Window" (ON by default) sizes the plugin window to
    // displayScale * the logical size it reports via onSize, while
    // GetDpiForWindow on the parent still reports 96 — so every input above
    // says 1.0 and the WebView fills only the top-left of the window (white
    // right/bottom margins). The client rect is the only ground truth, so
    // when it disagrees with our computed bounds by a UNIFORM factor, size
    // the WebView to the real window and fold the factor into the WebView2
    // zoom (crisp CSS-pixel re-raster, not a bitmap stretch). Hosts whose
    // client rect matches the logical size (older Ableton, Auto-Scale OFF,
    // 100% scaling, Reaper, Bitwig, ...) measure factor ~1.0 and skip this
    // block entirely — behavior identical to before. The FL (-1) and Cubase
    // (-2) branches are intentionally not reconciled: FL virtualizes the
    // client rect to logical units, so a mismatch there is expected.
    //
    // CRITICAL: measure the parent client rect TWICE, in two thread DPI
    // contexts (probed empirically on Ableton 12.4 / 125%):
    //  - physicalRect, read under PER_MONITOR_AWARE_V2: physical pixels —
    //    the units SetBoundsAndZoomFactor actually consumes (the WebView's
    //    clip window lands at exactly the bounds we pass, in these units).
    //  - internalRect, read under the parent window's own context: the
    //    window's internal, possibly DPI-virtualized units.
    // Ableton's Auto-Scale hosts the plugin window DPI-UNAWARE and lets DWM
    // stretch it (internal 1200x800 displayed as 1500x1000 physical), while
    // Chromium's render surface is sized bounds x monitorScale even though
    // every DPI API in reach lies (GetDpiForWindow says 96, WebView2's
    // RasterizationScale API reports 1.0). The ratio of the two reads is
    // the real virtualization scale, measured rather than queried — 1.0 on
    // every host that doesn't play this game.
    RECT physicalRect = {};
    RECT internalRect = {};
    {
      using GetWindowDpiCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
      using SetThreadDpiCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
      static GetWindowDpiCtxFn pGetWindowDpiCtx = []() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetWindowDpiCtxFn>(GetProcAddress(user32, "GetWindowDpiAwarenessContext")) : nullptr;
      }();
      static SetThreadDpiCtxFn pSetThreadDpiCtx = []() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<SetThreadDpiCtxFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")) : nullptr;
      }();

      if (pGetWindowDpiCtx && pSetThreadDpiCtx)
      {
        DPI_AWARENESS_CONTEXT prev = pSetThreadDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        GetClientRect(mParentWnd, &physicalRect);
        pSetThreadDpiCtx(pGetWindowDpiCtx(mParentWnd));
        GetClientRect(mParentWnd, &internalRect);
        pSetThreadDpiCtx(prev);
      }
      else
      {
        // pre-1607 Windows: no per-window contexts, no virtualization games
        GetClientRect(mParentWnd, &physicalRect);
        internalRect = physicalRect;
      }
    }
    const LONG clientW = physicalRect.right - physicalRect.left;
    const LONG clientH = physicalRect.bottom - physicalRect.top;
    const LONG internalW = internalRect.right - internalRect.left;
    const LONG boundsW = mWebViewBounds.right - mWebViewBounds.left;
    const LONG boundsH = mWebViewBounds.bottom - mWebViewBounds.top;

    if (x == 0.f && y == 0.f && clientW > 0 && clientH > 0 && boundsW > 0 && boundsH > 0)
    {
      const float fx = static_cast<float>(clientW) / static_cast<float>(boundsW);
      const float fy = static_cast<float>(clientH) / static_cast<float>(boundsH);
      const bool uniform = std::fabs(fx - fy) < 0.02f; // a scale, not an unrelated relayout
      const bool nonTrivial = std::fabs(fx - 1.f) > 0.05f; // ignore the +1 slop in GetScaledRect
      const bool sane = fx > 0.5f && fx < 4.f; // plausible display-scale range

      if (uniform && nonTrivial && sane)
      {
        float monitorScale = (internalW > 0) ? static_cast<float>(clientW) / static_cast<float>(internalW) : 1.f;
        if (!(monitorScale > 0.5f && monitorScale < 4.f))
          monitorScale = 1.f;

        // Fill the parent's physical client rect, and counter Chromium's
        // hidden monitorScale rasterization so the design ends up displayed
        // edge-to-edge: effective on-screen CSS pixel = monitorScale x zoom,
        // so zoom = f / monitorScale makes design x cssScale x monitorScale
        // x zoom == physical client. On Ableton 12.4 @125% that is
        // 1.249 / 1.25 ~= 1.0; verified against three observed states
        // (margins, overzoomed crop, and a hand-corrected perfect render).
        mWebViewBounds = physicalRect;
        zoom *= fx / monitorScale;

        // On this (virtualized) path window.innerWidth/Height persistently lie
        // by monitorScale — the page measures the physical-derived viewport
        // while only measured/monitorScale CSS px are visible, and no
        // resize/dpr event ever corrects it. Publish the factor so the
        // viewport-fit divides its measurements by it (natural-size render,
        // the verified Ableton 12.4 state). 1.0 hosts never reach this block.
        if (mIWebView)
          mIWebView->SetViewportVirtScale(monitorScale);

        ::WebViewInitLog("SetWebViewBounds:reconciled", S_OK,
                         "f=%.3f monitorScale=%.3f clientPhys=%ldx%ld internal=%ldx%ld requested=%ldx%ld zoom=%.3f",
                         fx, monitorScale, clientW, clientH, internalW,
                         internalRect.bottom - internalRect.top, boundsW, boundsH, zoom);
      }
      else if (!nonTrivial && mIWebView)
      {
        // Bounds match the physical client (the normal-host signature) —
        // make sure a stale factor from a prior virtualized state can't
        // linger after e.g. toggling Auto-Scale off.
        mIWebView->SetViewportVirtScale(1.f);
      }
    }
  }

  // Visibility clamp (Studio One / Fender Studio Pro and similar). Two measured
  // host behaviors leave part of the WebView OUTSIDE the visible top-level
  // frame, clipped with no scrollbar and no callback to the plugin:
  //  - clean open on a short screen: the host clamps only the frame to the
  //    work area while building our parent container at full requested size;
  //  - cross-DPI monitor move: the host's chrome grows with the new DPI and
  //    our container is SHIFTED DOWN below it without being resized.
  // Neither is visible in GetClientRect(mParentWnd) — the parent keeps its full
  // size; only its position/overlap vs the GA_ROOT frame changes. So compute
  // the VISIBLE region — the intersection of the frame's client area and the
  // parent's client area, expressed in parent-client coordinates and measured
  // in the same PER_MONITOR_AWARE_V2 physical units SetBoundsAndZoomFactor
  // consumes — and clamp the bounds to it. Zoom is deliberately untouched:
  // shrinking the bounds shrinks the page viewport, and the delegate's
  // viewport-fit letterboxes the content inside it. A 2px deadband absorbs the
  // +1 slop in GetScaledRect so healthy opens stay byte-identical.
  if (HWND rootFrame = GetAncestor(mParentWnd, GA_ROOT))
  {
    RECT parentScreen = {};
    RECT frameScreen = {};
    bool measured = false;
    {
      using SetThreadDpiCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
      static SetThreadDpiCtxFn pSetThreadDpiCtx = []() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<SetThreadDpiCtxFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")) : nullptr;
      }();
      DPI_AWARENESS_CONTEXT prev = pSetThreadDpiCtx ? pSetThreadDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
      RECT pc = {}, fc = {};
      POINT pOrigin = { 0, 0 }, fOrigin = { 0, 0 };
      if (GetClientRect(mParentWnd, &pc) && ClientToScreen(mParentWnd, &pOrigin) &&
          GetClientRect(rootFrame, &fc) && ClientToScreen(rootFrame, &fOrigin) &&
          pc.right > 0 && pc.bottom > 0 && fc.right > 0 && fc.bottom > 0)
      {
        parentScreen = { pOrigin.x, pOrigin.y, pOrigin.x + pc.right, pOrigin.y + pc.bottom };
        frameScreen = { fOrigin.x, fOrigin.y, fOrigin.x + fc.right, fOrigin.y + fc.bottom };
        measured = true;
      }
      if (pSetThreadDpiCtx)
        pSetThreadDpiCtx(prev);
    }

    if (measured)
    {
      RECT visScreen = {};
      if (IntersectRect(&visScreen, &parentScreen, &frameScreen))
      {
        // visible region in parent-client coordinates (the space of mWebViewBounds)
        const RECT vis = { visScreen.left - parentScreen.left, visScreen.top - parentScreen.top,
                           visScreen.right - parentScreen.left, visScreen.bottom - parentScreen.top };
        RECT clamped = {};
        if (IntersectRect(&clamped, &mWebViewBounds, &vis))
        {
          const bool differs = (clamped.left   > mWebViewBounds.left + 2)  ||
                               (clamped.top    > mWebViewBounds.top + 2)   ||
                               (clamped.right  < mWebViewBounds.right - 2) ||
                               (clamped.bottom < mWebViewBounds.bottom - 2);
          if (differs)
          {
            ::WebViewInitLog("SetWebViewBounds:visclamp", S_OK,
                             "bounds=(%ld,%ld %ldx%ld) visible=(%ld,%ld %ldx%ld) clamped=(%ld,%ld %ldx%ld)",
                             mWebViewBounds.left, mWebViewBounds.top,
                             mWebViewBounds.right - mWebViewBounds.left, mWebViewBounds.bottom - mWebViewBounds.top,
                             vis.left, vis.top, vis.right - vis.left, vis.bottom - vis.top,
                             clamped.left, clamped.top,
                             clamped.right - clamped.left, clamped.bottom - clamped.top);
            mWebViewBounds = clamped;
          }
        }
      }
    }
  }

  // Dedup: the parent-watch and resize storms re-run this often — only push
  // (and log) when the effective bounds or zoom actually changed.
  if (EqualRect(&mWebViewBounds, &mLastPushedBounds) && std::fabs(zoom - mLastPushedZoom) < 0.0005)
    return false;
  mLastPushedBounds = mWebViewBounds;
  mLastPushedZoom = zoom;

  mWebViewCtrlr->SetBoundsAndZoomFactor(mWebViewBounds, zoom);

  ::WebViewInitLog("SetWebViewBounds:applied", S_OK,
                   "w=%.0f h=%.0f scale=%.2f dpiScale=%.2f rect=%ldx%ld zoom=%.3f",
                   w, h, scale, dpiScale,
                   mWebViewBounds.right - mWebViewBounds.left,
                   mWebViewBounds.bottom - mWebViewBounds.top, zoom);
  return true;
}

void IWebViewImpl::PinRasterizationScale()
{
  if (!mWebViewCtrlr || !mParentWnd)
    return;

  auto ctrlr3 = mWebViewCtrlr.try_query<ICoreWebView2Controller3>();
  if (!ctrlr3)
    return;

  // Hosts that DPI-virtualize the plugin window (Ableton Auto-Scale: window
  // internally 1200x800 but displayed 1500x1000, every DPI API lying) depend
  // on Chromium's hidden monitor-scale rasterization, which the zoom
  // reconcile in SetWebViewBounds counters (zoom = f / monitorScale). Pinning
  // the rasterization scale on such a host kills that hidden scale AFTER the
  // page already measured its viewport — and with detection off no
  // resize/dpr event ever fires for the correction — so the fit stays
  // computed against the pre-settle viewport and the content renders
  // overzoomed and cropped (the exact failure state the Ableton 12.4
  // reconcile fixed). Measure the virtualization ratio the same dual-context
  // way the reconcile does and skip the pin entirely on virtualized hosts;
  // their proven-good path is Chromium auto-detection + the zoom reconcile.
  // The ratio is ~1.0 on every host that doesn't play this game (Studio One,
  // Fender Studio Pro, Reaper, ...), so those keep the pin unchanged.
  {
    using GetWindowDpiCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
    using SetThreadDpiCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static GetWindowDpiCtxFn pGetWindowDpiCtx = []() {
      HMODULE user32 = GetModuleHandleW(L"user32.dll");
      return user32 ? reinterpret_cast<GetWindowDpiCtxFn>(GetProcAddress(user32, "GetWindowDpiAwarenessContext")) : nullptr;
    }();
    static SetThreadDpiCtxFn pSetThreadDpiCtx = []() {
      HMODULE user32 = GetModuleHandleW(L"user32.dll");
      return user32 ? reinterpret_cast<SetThreadDpiCtxFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")) : nullptr;
    }();

    if (pGetWindowDpiCtx && pSetThreadDpiCtx)
    {
      RECT physicalRect = {};
      RECT internalRect = {};
      DPI_AWARENESS_CONTEXT prev = pSetThreadDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      GetClientRect(mParentWnd, &physicalRect);
      pSetThreadDpiCtx(pGetWindowDpiCtx(mParentWnd));
      GetClientRect(mParentWnd, &internalRect);
      pSetThreadDpiCtx(prev);

      const LONG physW = physicalRect.right - physicalRect.left;
      const LONG intW = internalRect.right - internalRect.left;
      if (physW > 0 && intW > 0)
      {
        const float virtScale = static_cast<float>(physW) / static_cast<float>(intW);
        if (virtScale > 0.5f && virtScale < 4.f && std::fabs(virtScale - 1.f) > 0.05f)
        {
          ::WebViewInitLog("controller:raster_pin_skipped", S_OK,
                           "virtScale=%.3f (DPI-virtualized host — Chromium auto-scale + zoom reconcile own this path)",
                           virtScale);
          return;
        }
      }
    }
  }

  float scale = GetScaleForHWND(mParentWnd);
  if (scale <= 0.f)
    scale = 1.f;

  // One source of truth for the page's devicePixelRatio: the parent window's
  // DPI. Auto-detection is disabled so WebView2 can never re-rasterize behind
  // our back on a monitor/DPI change — we re-pin explicitly instead.
  ctrlr3->put_ShouldDetectMonitorScaleChanges(FALSE);

  double current = 0.0;
  ctrlr3->get_RasterizationScale(&current);
  if (std::fabs(current - static_cast<double>(scale)) > 0.01)
  {
    ctrlr3->put_RasterizationScale(static_cast<double>(scale));
    ::WebViewInitLog("controller:raster_pinned", S_OK, "raster=%.2f (was %.2f)", scale, current);
  }
}

void IWebViewImpl::SnapFrameToContent()
{
  // Collapse dead letterbox bands: when the host centers our (aspect-corrected,
  // smaller) container inside the window the user dragged — painting the
  // leftover client area as black bands — shrink the host frame so its client
  // hugs chrome + container exactly. Shrink-only, >=4px deadband, converges in
  // one step (the resulting WM_SIZE re-measures to delta 0). Runs debounced
  // after layout settles, never during the interactive drag.
  if (!mParentWnd || !mSubclassedHwnd || mInSizeMove)
  {
    ::WebViewInitLog("FrameSnap:skip", S_OK, "parent=%p frame=%p inSizeMove=%d",
                     (void*)mParentWnd, (void*)mSubclassedHwnd, mInSizeMove ? 1 : 0);
    return;
  }
  if (GetAncestor(mParentWnd, GA_ROOT) != mSubclassedHwnd)
  {
    ::WebViewInitLog("FrameSnap:skip", S_OK, "reparented (root=%p != hooked frame=%p)",
                     (void*)GetAncestor(mParentWnd, GA_ROOT), (void*)mSubclassedHwnd);
    return;
  }

  using SetThreadDpiCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
  static SetThreadDpiCtxFn pSetThreadDpiCtx = []() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<SetThreadDpiCtxFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")) : nullptr;
  }();

  DPI_AWARENESS_CONTEXT prev = pSetThreadDpiCtx ? pSetThreadDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;

  RECT frameClient = {}, frameWindow = {}, containerWindow = {};
  POINT frameOrigin = { 0, 0 };
  const bool ok = GetClientRect(mSubclassedHwnd, &frameClient) && ClientToScreen(mSubclassedHwnd, &frameOrigin) &&
                  GetWindowRect(mSubclassedHwnd, &frameWindow) && GetWindowRect(mParentWnd, &containerWindow);

  bool snapped = false;
  int deltaW = 0, deltaH = 0;
  if (ok && frameClient.right > 0 && frameClient.bottom > 0 && mDesignWidth > 0 && mDesignHeight > 0)
  {
    const int containerW = containerWindow.right - containerWindow.left;
    const int containerH = containerWindow.bottom - containerWindow.top;
    const int topChrome = containerWindow.top - frameOrigin.y; // host UI strip above the plugin — keep it

    if (containerW > 0 && containerH > 0 && topChrome >= 0)
    {
      // Target = the design-aspect contain-fit of the container: where the
      // CONTENT actually ends (the viewport-fit letterboxes inside the page).
      // Trimming to the content collapses both the host's dead bands around a
      // smaller view AND the in-page letterbox in one step.
      const float fitX = static_cast<float>(containerW) / static_cast<float>(mDesignWidth);
      const float fitY = static_cast<float>(containerH) / static_cast<float>(mDesignHeight);
      const float fit = (fitX < fitY) ? fitX : fitY;
      const int targetW = static_cast<int>(mDesignWidth * fit + 0.5f);
      const int targetH = static_cast<int>(mDesignHeight * fit + 0.5f);

      deltaW = frameClient.right - targetW;                // dead band + letterbox, horizontally
      deltaH = frameClient.bottom - (topChrome + targetH); // same, below the content

      // Collapse each axis's dead band INDEPENDENTLY. A small negative delta is
      // a rounding artifact (measured live: deltaH=-1 while deltaW=193) — it
      // must NOT veto the other axis's real band, so clamp it to zero. Only a
      // genuinely negative delta (content overflows the frame by more than the
      // rounding slop) is clip territory, owned by the visibility clamp; leave
      // that axis alone rather than shrink into a clip.
      const int kSlop = 8;
      const int shrinkW = (deltaW > 4) ? deltaW : 0;
      const int shrinkH = (deltaH > 4) ? deltaH : 0;
      if ((shrinkW > 0 && deltaH > -kSlop) || (shrinkH > 0 && deltaW > -kSlop))
      {
        const int applyW = (deltaW > -kSlop) ? shrinkW : 0;
        const int applyH = (deltaH > -kSlop) ? shrinkH : 0;
        const int newW = (frameWindow.right - frameWindow.left) - applyW;
        const int newH = (frameWindow.bottom - frameWindow.top) - applyH;
        SetWindowPos(mSubclassedHwnd, nullptr, 0, 0, newW, newH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        deltaW = applyW; deltaH = applyH; // report what we actually applied
        snapped = true;
      }
    }
  }

  if (pSetThreadDpiCtx)
    pSetThreadDpiCtx(prev);

  if (snapped)
    ::WebViewInitLog("FrameSnap:applied", S_OK, "deltaW=%d deltaH=%d", deltaW, deltaH);
  else
    ::WebViewInitLog("FrameSnap:noop", S_OK, "ok=%d deltaW=%d deltaH=%d frameClient=%ldx%ld",
                     ok ? 1 : 0, deltaW, deltaH, frameClient.right, frameClient.bottom);
}

bool IWebViewImpl::NudgeClippedAncestorIntoView()
{
  // FL Studio hosts attached plugin windows as movable CHILD windows inside
  // its client area, and places a first-open wrapper so its bottom hangs
  // below the frame's client region — clipped, with no notification, and
  // never corrected by the host. Users fix it by dragging the wrapper up;
  // this does the same programmatically, once, at open-settle. The walk
  // stops at GA_ROOT, so hosts whose wrapper IS the top-level window
  // (Studio One, FSP, ...) can never be nudged by this path.
  if (!mParentWnd || mInSizeMove)
    return false;
  HWND root = GetAncestor(mParentWnd, GA_ROOT);
  if (!root || root == mParentWnd)
    return false;

  using SetThreadDpiCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
  static SetThreadDpiCtxFn pSetThreadDpiCtx = []() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<SetThreadDpiCtxFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")) : nullptr;
  }();
  DPI_AWARENESS_CONTEXT prev = pSetThreadDpiCtx ? pSetThreadDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;

  bool nudged = false;
  RECT rootClient = {};
  POINT rootOrigin = { 0, 0 };
  RECT parentRect = {};
  if (GetClientRect(root, &rootClient) && ClientToScreen(root, &rootOrigin) &&
      GetWindowRect(mParentWnd, &parentRect) && rootClient.bottom > 0)
  {
    const LONG rootTop = rootOrigin.y;
    const LONG rootBottom = rootOrigin.y + rootClient.bottom;
    const LONG overhang = parentRect.bottom - rootBottom;
    if (overhang > 16)
    {
      // Highest ancestor BELOW the root whose bottom hangs past the root's
      // client region — in FL that is the draggable wrapper form itself
      // (its own parent, FL's layout panel, ends at the client bottom).
      HWND cand = NULL;
      RECT candRect = {};
      for (HWND cur = mParentWnd; cur && cur != root; cur = GetAncestor(cur, GA_PARENT))
      {
        RECT r = {};
        if (GetWindowRect(cur, &r) && r.bottom > rootBottom + 8)
        {
          cand = cur;
          candRect = r;
        }
      }
      if (cand)
      {
        LONG newTop = candRect.top - overhang - 8;
        if (newTop < rootTop + 4)
          newTop = rootTop + 4; // keep the caption reachable; partial visibility beats none
        if (newTop < candRect.top)
        {
          HWND candParent = GetAncestor(cand, GA_PARENT);
          POINT target = { candRect.left, newTop };
          if (candParent && ScreenToClient(candParent, &target) &&
              SetWindowPos(cand, nullptr, target.x, target.y, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
          {
            nudged = true;
            ::WebViewInitLog("OpenSettle:nudge_up", S_OK, "hwnd=%p by=%ld overhang=%ld",
                             (void*)cand, candRect.top - newTop, overhang);
          }
        }
      }
    }
  }

  if (pSetThreadDpiCtx)
    pSetThreadDpiCtx(prev);
  return nudged;
}

void IWebViewImpl::GetLocalDownloadPathForFile(const char* fileName, WDL_String& downloadPath)
{
  DesktopPath(downloadPath);
  downloadPath.Append(fileName);
}

#include "IPlugWebView.cpp"