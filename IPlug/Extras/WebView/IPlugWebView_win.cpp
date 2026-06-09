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
  void InstallAspectRatioHook(int designWidth, int designHeight);
  void RemoveAspectRatioHook();
  void ApplyWebViewBounds();
public:
  void SetMinSize(int minW, int minH) { mMinWidth = minW; mMinHeight = minH; }
private:

  IWebView* mIWebView;
  bool mOpaque;
  HWND mParentWnd = NULL;
  HWND mSubclassedHwnd = NULL;
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

LRESULT CALLBACK IWebViewImpl::AspectRatioSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
  if (msg == WM_NCDESTROY)
  {
    RemoveWindowSubclass(hWnd, &IWebViewImpl::AspectRatioSubclassProc, uIdSubclass);
    return DefSubclassProc(hWnd, msg, wParam, lParam);
  }

  if ((msg != WM_SIZING && msg != WM_GETMINMAXINFO) || !dwRefData)
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
    const int ncW = (windowRect.right - windowRect.left) - clientRect.right;
    const int ncH = (windowRect.bottom - windowRect.top) - clientRect.bottom;

    MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
    mmi->ptMinTrackSize.x = minW + ncW;
    mmi->ptMinTrackSize.y = minH + ncH;
    return 0;
  }

  // Below here: msg == WM_SIZING

  RECT* rect = reinterpret_cast<RECT*>(lParam);
  // Subtract non-client (title bar, borders) so aspect applies to the client
  // area only — otherwise the title bar skews the ratio.
  RECT windowRect, clientRect;
  GetWindowRect(hWnd, &windowRect);
  GetClientRect(hWnd, &clientRect);
  const int ncW = (windowRect.right - windowRect.left) - clientRect.right;
  const int ncH = (windowRect.bottom - windowRect.top) - clientRect.bottom;

  const int draggedW = (rect->right - rect->left) - ncW;
  const int draggedH = (rect->bottom - rect->top) - ncH;
  if (draggedW <= 0 || draggedH <= 0)
    return DefSubclassProc(hWnd, msg, wParam, lParam);

  const float aspect = static_cast<float>(designW) / static_cast<float>(designH);
  int newW = draggedW;
  int newH = draggedH;

  switch (wParam)
  {
    case WMSZ_LEFT:
    case WMSZ_RIGHT:
      // Horizontal edge drag — keep width, adjust height to match aspect.
      newH = static_cast<int>(static_cast<float>(draggedW) / aspect + 0.5f);
      break;
    case WMSZ_TOP:
    case WMSZ_BOTTOM:
      // Vertical edge drag — keep height, adjust width to match aspect.
      newW = static_cast<int>(static_cast<float>(draggedH) * aspect + 0.5f);
      break;
    case WMSZ_TOPLEFT:
    case WMSZ_TOPRIGHT:
    case WMSZ_BOTTOMLEFT:
    case WMSZ_BOTTOMRIGHT:
    {
      // Corner drag — pick the dimension that was dragged more aggressively
      // relative to the design aspect, and clamp the other to match.
      const float draggedAspect = static_cast<float>(draggedW) / static_cast<float>(draggedH);
      if (draggedAspect > aspect)
        newW = static_cast<int>(static_cast<float>(draggedH) * aspect + 0.5f);
      else
        newH = static_cast<int>(static_cast<float>(draggedW) / aspect + 0.5f);
      break;
    }
    default:
      return DefSubclassProc(hWnd, msg, wParam, lParam);
  }

  // Clamp to the plugin's minimum size while preserving aspect ratio. If
  // either dimension would go below its minimum, rescale both so the smaller
  // dimension sits exactly at its minimum — this keeps the resize smooth
  // instead of snapping to sub-minimum sizes.
  if (newW < minW || newH < minH)
  {
    const float scaleW = static_cast<float>(minW) / static_cast<float>(newW);
    const float scaleH = static_cast<float>(minH) / static_cast<float>(newH);
    const float scaleUp = (scaleW > scaleH) ? scaleW : scaleH;
    newW = static_cast<int>(static_cast<float>(newW) * scaleUp + 0.5f);
    newH = static_cast<int>(static_cast<float>(newH) * scaleUp + 0.5f);
  }

  // Anchor the non-moving edge, move the other to apply the corrected dims.
  switch (wParam)
  {
    case WMSZ_LEFT:
      rect->left = rect->right - (newW + ncW);
      rect->bottom = rect->top + newH + ncH;
      break;
    case WMSZ_RIGHT:
      rect->right = rect->left + newW + ncW;
      rect->bottom = rect->top + newH + ncH;
      break;
    case WMSZ_TOP:
      rect->top = rect->bottom - (newH + ncH);
      rect->right = rect->left + newW + ncW;
      break;
    case WMSZ_BOTTOM:
      rect->bottom = rect->top + newH + ncH;
      rect->right = rect->left + newW + ncW;
      break;
    case WMSZ_TOPLEFT:
      rect->top = rect->bottom - (newH + ncH);
      rect->left = rect->right - (newW + ncW);
      break;
    case WMSZ_TOPRIGHT:
      rect->top = rect->bottom - (newH + ncH);
      rect->right = rect->left + newW + ncW;
      break;
    case WMSZ_BOTTOMLEFT:
      rect->bottom = rect->top + newH + ncH;
      rect->left = rect->right - (newW + ncW);
      break;
    case WMSZ_BOTTOMRIGHT:
      rect->bottom = rect->top + newH + ncH;
      rect->right = rect->left + newW + ncW;
      break;
  }

  return TRUE;
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
}

void IWebViewImpl::RemoveAspectRatioHook()
{
  if (mSubclassedHwnd)
  {
    RemoveWindowSubclass(mSubclassedHwnd, &IWebViewImpl::AspectRatioSubclassProc, kAspectRatioSubclassId);
    mSubclassedHwnd = NULL;
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

            // Replay the latest size request now that the controller exists.
            // The host may have (re)sized the parent window during the async
            // controller creation, so recompute against the live client rect
            // instead of pushing the possibly-stale mWebViewBounds.
            if (mHasLastBounds)
              ApplyWebViewBounds();
            else
              mWebViewCtrlr->put_Bounds(mWebViewBounds);
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
  if (mCoreWebView)
  {
    int bufSize = UTF8ToUTF16Len(scriptStr);
    std::vector<WCHAR> scriptWide(bufSize);
    UTF8ToUTF16(scriptWide.data(), scriptStr, bufSize);

    mCoreWebView->ExecuteScript(
      scriptWide.data(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([func](HRESULT errorCode,
                                                                              LPCWSTR resultObjectAsJson) -> HRESULT {
                    if (func && resultObjectAsJson)
                    {
                      WDL_String str;
                      UTF16ToUTF8(str, resultObjectAsJson);
                      func(str.Get());
                    }
                    return S_OK;
                  }).Get());
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

void IWebViewImpl::ApplyWebViewBounds()
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
    return;

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
    RECT clientRect = {};
    GetClientRect(mParentWnd, &clientRect);
    const LONG clientW = clientRect.right - clientRect.left;
    const LONG clientH = clientRect.bottom - clientRect.top;
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
        mWebViewBounds = clientRect;
        zoom *= fx;
        ::WebViewInitLog("SetWebViewBounds:reconciled", S_OK,
                         "f=%.3f client=%ldx%ld requested=%ldx%ld zoom=%.3f",
                         fx, clientW, clientH, boundsW, boundsH, zoom);
      }
    }
  }

  mWebViewCtrlr->SetBoundsAndZoomFactor(mWebViewBounds, zoom);

  ::WebViewInitLog("SetWebViewBounds:applied", S_OK,
                   "w=%.0f h=%.0f scale=%.2f dpiScale=%.2f rect=%ldx%ld zoom=%.3f",
                   w, h, scale, dpiScale,
                   mWebViewBounds.right - mWebViewBounds.left,
                   mWebViewBounds.bottom - mWebViewBounds.top, zoom);
}

void IWebViewImpl::GetLocalDownloadPathForFile(const char* fileName, WDL_String& downloadPath)
{
  DesktopPath(downloadPath);
  downloadPath.Append(fileName);
}

#include "IPlugWebView.cpp"