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

#include "IPlugEditorDelegate.h"
#include "IPlugWebView.h"
#include "wdl_base64.h"
#include "json.hpp"
#include <functional>
#include <filesystem>
#include <cstdio>
#include <cmath>
#ifdef OS_WIN
#include <windows.h>
#endif

/**
 * @file
 * @copydoc WebViewEditorDelegate
 */

BEGIN_IPLUG_NAMESPACE

#if defined APP_API
// Standalone (APP) device-picker bridge — implemented in IPlugAPP_host.cpp so
// this generic WebView header stays free of the RtAudio/RtMidi host headers.
// PMGetDeviceListScript() returns a JS snippet calling window.PMOnDevices({...}).
std::string PMGetDeviceListScript();
void PMSelectAudioOutputDevice(const char* name);
void PMSelectMIDIInputDevice(const char* name);
#endif

/** An editor delegate base class that uses a platform native webview for the UI
* @ingroup EditorDelegates */
class WebViewEditorDelegate : public IEditorDelegate
                            , public IWebView
{
  static constexpr int kDefaultMaxJSStringLength = 8192;
  
public:
  WebViewEditorDelegate(int nParams);
  virtual ~WebViewEditorDelegate();
  
  //IEditorDelegate
  void* OpenWindow(void* pParent) override;
  
  void CloseWindow() override
  {
    CloseWebView();
  }

  void SendControlValueFromDelegate(int ctrlTag, double normalizedValue) override
  {
    WDL_String str;
    str.SetFormatted(mMaxJSStringLength, "SCVFD(%i, %f)", ctrlTag, normalizedValue);
    EvaluateJavaScript(str.Get());
  }

  void SendControlMsgFromDelegate(int ctrlTag, int msgTag, int dataSize, const void* pData) override
  {
    WDL_String str;
    std::vector<char> base64;
    base64.resize(GetBase64Length(dataSize) + 1);
    wdl_base64encode(reinterpret_cast<const unsigned char*>(pData), base64.data(), dataSize);
    str.SetFormatted(mMaxJSStringLength, "SCMFD(%i, %i, %i, '%s')", ctrlTag, msgTag, dataSize, base64.data());
    EvaluateJavaScript(str.Get());
  }

  void SendParameterValueFromDelegate(int paramIdx, double value, bool normalized) override
  {
    WDL_String str;
    
    if (!normalized)
    {
      value = GetParam(paramIdx)->ToNormalized(value);
    }
    
    str.SetFormatted(mMaxJSStringLength, "SPVFD(%i, %f)", paramIdx, value);
    EvaluateJavaScript(str.Get());
  }

  void SendArbitraryMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    WDL_String str;
    std::vector<char> base64;
    if (dataSize)
    {
      base64.resize(GetBase64Length(dataSize) + 1);
      wdl_base64encode(reinterpret_cast<const unsigned char*>(pData), base64.data(), dataSize);
    }
    str.SetFormatted(mMaxJSStringLength, "SAMFD(%i, %i, '%s')", msgTag, static_cast<int>(base64.size()), base64.data());
    EvaluateJavaScript(str.Get());
  }
  
  void SendMidiMsgFromDelegate(const IMidiMsg& msg) override
  {
    WDL_String str;
    str.SetFormatted(mMaxJSStringLength, "SMMFD(%i, %i, %i)", msg.mStatus, msg.mData1, msg.mData2);
    EvaluateJavaScript(str.Get());
  }
  
  bool OnKeyDown(const IKeyPress& key) override;
  bool OnKeyUp(const IKeyPress& key) override;

  // IWebView

  void SendJSONFromDelegate(const nlohmann::json& jsonMessage)
  {
    SendArbitraryMsgFromDelegate(-1, static_cast<int>(jsonMessage.dump().size()), jsonMessage.dump().c_str());
  }

  void OnMessageFromWebView(const char* jsonStr) override
  {
    auto json = nlohmann::json::parse(jsonStr, nullptr, false);

    if (json["msg"] == "SPVFUI")
    {
      assert(json["paramIdx"] > -1);
      SendParameterValueFromUI(json["paramIdx"], json["value"]);
    }
    else if (json["msg"] == "BPCFUI")
    {
      assert(json["paramIdx"] > -1);
      BeginInformHostOfParamChangeFromUI(json["paramIdx"]);
    }
    else if (json["msg"] == "EPCFUI")
    {
      assert(json["paramIdx"] > -1);
      EndInformHostOfParamChangeFromUI(json["paramIdx"]);
    }
    else if (json["msg"] == "SAMFUI")
    {
      std::vector<unsigned char> base64;

      if(json.count("data") > 0 && json["data"].is_string())
      {
        auto dStr = json["data"].get<std::string>();
        int dSize = static_cast<int>(dStr.size());
        
        // calculate the exact size of the decoded base64 data
        int numPaddingBytes = 0;
        
        if(dSize >= 2 && dStr[dSize-2] == '=')
          numPaddingBytes = 2;
        else if(dSize >= 1 && dStr[dSize-1] == '=')
          numPaddingBytes = 1;
        
        base64.resize((dSize * 3) / 4 - numPaddingBytes);
        wdl_base64decode(dStr.c_str(), base64.data(), static_cast<int>(base64.size()));
      }

      SendArbitraryMsgFromUI(json["msgTag"], json["ctrlTag"], static_cast<int>(base64.size()), base64.data());
    }
    else if(json["msg"] == "SMMFUI")
    {
      IMidiMsg msg {0, json["statusByte"].get<uint8_t>(),
                       json["dataByte1"].get<uint8_t>(),
                       json["dataByte2"].get<uint8_t>()};
      SendMidiMsgFromUI(msg);
    }
    else if(json["msg"] == "SKPFUI")
    {
      IKeyPress keyPress = ConvertToIKeyPress(json["keyCode"].get<uint32_t>(), json["utf8"].get<std::string>().c_str(), json["S"].get<bool>(), json["C"].get<bool>(), json["A"].get<bool>());
      json["isUp"].get<bool>() ? OnKeyUp(keyPress) : OnKeyDown(keyPress); // return value not used
    }
#if defined APP_API
    else if(json["msg"] == "PMRDV") // standalone: request audio/MIDI device list
    {
      std::string script = PMGetDeviceListScript();
      EvaluateJavaScript(script.c_str(), nullptr);
    }
    else if(json["msg"] == "PMSAO") // standalone: select audio output device
    {
      PMSelectAudioOutputDevice(json.value("name", std::string()).c_str());
    }
    else if(json["msg"] == "PMSMI") // standalone: select MIDI input device
    {
      PMSelectMIDIInputDevice(json.value("name", std::string()).c_str());
    }
#endif
  }

  void Resize(int width, int height);

  void OnParentWindowResize(int width, int height) override;

#ifdef OS_WIN
  // Fit the web content to the WebView's ACTUAL viewport, measured live in JS
  // (window.innerWidth/innerHeight), instead of a scale derived from mScreenScale.
  // A stale/wrong mScreenScale — e.g. after a mixed-DPI monitor move, or a host
  // that reports a scale which doesn't match the WebView's rasterization — made
  // the old path over-scale the content by (realDPI / mScreenScale), overflowing
  // the viewport and clipping the bottom of the UI (Studio One / FSP at high DPI).
  // Measuring the viewport is correct by construction and self-installs a resize
  // listener so it re-fits on open, drag-resize, and DPI change alike.
  // Windows-only: macOS compiles IPlugWebViewEditorDelegate.mm (which has no
  // InjectViewportFit definition — declaring it here broke the mac link) and
  // handles sizing natively via setContentAspectRatio. Mac keeps its verified
  // v106 behavior untouched.
  void InjectViewportFit();

  bool ConstrainEditorResize(int& w, int& h) const override
  {
    // Accept whatever the host proposes — do NOT correct it here. Corrections
    // returned from checkSizeConstraint make hosts like Studio One shrink and
    // center the plugin view inside the window the user is dragging, painting
    // the leftover client area as dead black bands. Content correctness is
    // owned downstream instead: the injected viewport-fit letterboxes the
    // content to the design aspect (centered, page background) inside whatever
    // rect arrives, the visibility clamp keeps it on-screen, and the Windows
    // frame snap trims the host window to hug the content once the interaction
    // settles. (Return true = "use the caller's original values".)
    return true;
  }
#else
  bool ConstrainEditorResize(int& w, int& h) const override
  {
    // macOS: unchanged v106 behavior — width-driven aspect correction. The
    // return value is inverted: `checkSizeConstraint` only writes w/h back to
    // the host's ViewRect when this returns false (meaning "I modified the
    // values"), so when we enforce aspect ratio we MUST return false to get
    // the correction through.
    if (mDesignWidth > 0 && mDesignHeight > 0)
    {
      float aspectRatio = static_cast<float>(mDesignWidth) / static_cast<float>(mDesignHeight);
      int newH = static_cast<int>(std::round(static_cast<float>(w) / aspectRatio));
      if (newH >= GetMinHeight() && newH <= GetMaxHeight())
      {
        h = newH;
      }
      else
      {
        h = Clip(newH, GetMinHeight(), GetMaxHeight());
        w = static_cast<int>(std::round(static_cast<float>(h) * aspectRatio));
      }
      return false;
    }
    return IEditorDelegate::ConstrainEditorResize(w, h);
  }
#endif

#ifdef OS_WIN
  void SetScreenScale(float scale) override
  {
    mScreenScale = scale;
    // Capture design dimensions before any host resizing (onSize) modifies them.
    // setContentScaleFactor is called before onSize, so GetEditorWidth() still
    // has the original PLUG_WIDTH/PLUG_HEIGHT values here.
    if (mDesignWidth == 0)
    {
      mDesignWidth = GetEditorWidth();
      mDesignHeight = GetEditorHeight();
    }
  }

  void SetDpiZoomCompensation(bool needed, const char* hostName = "unknown") override
  {
    mNeedsDpiZoomCompensation = needed;
  }
#endif

  void OnWebViewReady() override
  {
    if (mEditorInitFunc)
    {
      mEditorInitFunc();
    }
  }
  
  void OnWebContentLoaded() override
  {
    nlohmann::json msg;
    
    msg["id"] = "params";
    std::vector<nlohmann::json> params;
    for (int idx = 0; idx < NParams(); idx++)
    {
      WDL_String jsonStr;
      IParam* pParam = GetParam(idx);
      pParam->GetJSON(jsonStr, idx);
      nlohmann::json paramMsg = nlohmann::json::parse(jsonStr.Get(), nullptr, true);
      params.push_back(paramMsg);
    }
    msg["params"] = params;

    SendJSONFromDelegate(msg);

    OnUIOpen();

#ifdef OS_WIN
    // Fit the content to the viewport on load, and install the resize/DPI
    // listener — so the UI is correctly scaled on a fresh open even for hosts
    // that don't send an initial onSize/OnParentWindowResize (and re-fits on
    // every subsequent resize or monitor-DPI change on its own).
    InjectViewportFit();
#endif
  }

#ifdef OS_WIN
  void OnWebViewViewportChanged() override
  {
    // Native viewport changed underneath the page (DPI change / visibility
    // clamp) — re-measure and re-fit without waiting for a host callback.
    InjectViewportFit();
  }
#endif
  
  void SetMaxJSStringLength(int length)
  {
    mMaxJSStringLength = length;
  }

  /** Load index.html (from plugin src dir in debug builds, and from bundle in release builds) on desktop
   * Note: if your debug build is code-signed with the hardened runtime It won't be able to load the file outside it's sandbox, and this
   * will fail.
   * On iOS, this will load index.html from the bundle
   * @param pathOfPluginSrc - path to the plugin src directory
   * @param bundleid - the bundle id, used to load the correct index.html from the bundle
   */
  void LoadIndexHtml(const char* pathOfPluginSrc, const char* bundleid)
  {
#if !defined OS_IOS && defined _DEBUG
    namespace fs = std::filesystem;
    
    fs::path mainPath(pathOfPluginSrc);
    fs::path indexRelativePath = mainPath.parent_path() / "Resources" / "web" / "index.html";

    LoadFile(indexRelativePath.string().c_str(), nullptr);
#else
    LoadFile("index.html", bundleid); // TODO: make this work for windows
#endif
  }

protected:
  int mMaxJSStringLength = kDefaultMaxJSStringLength;
  std::function<void()> mEditorInitFunc = nullptr;
  void* mView = nullptr;
#ifdef OS_WIN
  unsigned int mLastSpaceForwardMs = 0; // tick of last spacebar->host forward (key-echo guard)
#endif
  int mDesignWidth = 0;  // Initial PLUG_WIDTH, used for pageZoom scaling on resize
  int mDesignHeight = 0;
  float mScreenScale = 1.f;
  bool mNeedsDpiZoomCompensation = false;
  
private:
  IKeyPress ConvertToIKeyPress(uint32_t keyCode, const char* utf8, bool shift, bool ctrl, bool alt)
  {
    return IKeyPress(utf8, DOMKeyToVirtualKey(keyCode), shift,ctrl, alt);
  }

  static int GetBase64Length(int dataSize)
  {
    return static_cast<int>(4. * std::ceil((static_cast<double>(dataSize) / 3.)));
  }

#if defined OS_MAC || defined OS_IOS
  void ResizeWebViewAndHelper(float width, float height);
#endif
};

END_IPLUG_NAMESPACE
