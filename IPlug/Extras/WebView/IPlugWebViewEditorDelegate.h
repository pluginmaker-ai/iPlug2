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
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>
#ifdef OS_WIN
#include <windows.h>
#endif

/**
 * @file
 * @copydoc WebViewEditorDelegate
 */

BEGIN_IPLUG_NAMESPACE

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
    else if(json["msg"] == "EXPMI")
    {
      // Export MIDI from UI: write the supplied SMF bytes to a temp file
      // and start an OS-level drag session so the user can drop the file
      // onto the host (DAW timeline, Finder). Filename comes from JS but
      // is restricted to a safe charset on this side too — the JS helper
      // already validates, this is defense in depth.
      HandleExportMidiDrag(json);
    }
  }

private:
  static bool IsValidDragFilenameStem(const std::string& name)
  {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name)
    {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (!(std::isalnum(uc) || c == '-' || c == '_')) return false;
    }
    return true;
  }

  void HandleExportMidiDrag(const nlohmann::json& json)
  {
    auto filenameIt = json.find("filename");
    auto base64It = json.find("base64");
    if (filenameIt == json.end() || !filenameIt->is_string()) return;
    if (base64It == json.end() || !base64It->is_string()) return;

    const std::string& filename = filenameIt->get_ref<const std::string&>();
    if (!IsValidDragFilenameStem(filename)) return;

    const std::string& b64 = base64It->get_ref<const std::string&>();
    const int b64Size = static_cast<int>(b64.size());
    if (b64Size == 0 || b64Size > kMaxDragPayloadBase64Bytes) return;

    int numPaddingBytes = 0;
    if (b64Size >= 2 && b64[b64Size - 2] == '=') numPaddingBytes = 2;
    else if (b64Size >= 1 && b64[b64Size - 1] == '=') numPaddingBytes = 1;
    const int decodedSize = (b64Size * 3) / 4 - numPaddingBytes;
    if (decodedSize <= 0) return;

    std::vector<unsigned char> decoded(decodedSize);
    wdl_base64decode(b64.c_str(), decoded.data(), decodedSize);

    namespace fs = std::filesystem;
    // std::filesystem::temp_directory_path() resolves to NSTemporaryDirectory
    // on macOS and is sandbox-safe in AU/VST3 hosts. The unique subdirectory
    // keeps each drag's file isolated so concurrent drags can't clobber one
    // another, and the JS-supplied basename ends up in the host's import
    // dialog (e.g. "drum-pattern.mid") instead of a UUID.
    std::error_code ec;
    fs::path baseDir = fs::temp_directory_path(ec);
    if (ec || baseDir.empty()) return;
    fs::path dragDir = baseDir / MakeUniqueDragDirName();
    fs::create_directories(dragDir, ec);
    if (ec) return;
    fs::path file = dragDir / (filename + ".mid");

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(decoded.data()), decodedSize);
    out.close();
    if (out.fail()) return;

    InitiateFileDrag(file.string().c_str());
  }

  static constexpr int kMaxDragPayloadBase64Bytes = 4 * 1024 * 1024; // 4 MB base64 ≈ 3 MB raw

  // 16 hex chars from a fresh random_device + counter. Not a real UUID
  // (don't need crypto-strength uniqueness — collision risk is one drag
  // overwriting another within the same plugin instance, which a 64-bit
  // value rules out for any realistic session length).
  static std::string MakeUniqueDragDirName()
  {
    static std::atomic<uint64_t> counter{0};
    std::random_device rd;
    uint64_t hi = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    uint64_t lo = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "iplug-drag-%016llx-%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf);
  }

public:

  void Resize(int width, int height);
  
  void OnParentWindowResize(int width, int height) override;

  bool ConstrainEditorResize(int& w, int& h) const override
  {
    // The return value is inverted: `checkSizeConstraint` only writes w/h back to
    // the host's ViewRect when this returns false (meaning "I modified the values").
    // Return true means "unchanged, use the caller's original values". So when we
    // enforce aspect ratio we MUST return false to get the correction through.
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

  void SetDpiZoomCompensation(bool needed, const char* hostName = "unknown")
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
  }
  
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
