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
    else if(json["msg"] == "RSZFUI")
    {
      int w = json["width"].get<int>();
      int h = json["height"].get<int>();
      if (w > 0 && h > 0)
        Resize(w, h);
    }
  }

  void Resize(int width, int height);
  
  void OnParentWindowResize(int width, int height) override;

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

    // Inject a resize handle for hosts that don't support host-initiated resize (e.g. AUv2 in Logic).
    // The handle sits in the bottom-right corner; dragging it sends RSZFUI messages to the C++ side
    // which calls Resize() → setFrameSize: → NSViewFrameDidChangeNotification → host follows.
    if (mDesignWidth > 0 && mDesignHeight > 0)
    {
      char resizeJs[2048];
      snprintf(resizeJs, sizeof(resizeJs),
        "(function(){"
        "if(document.getElementById('iplug-resize-handle'))return;"
        "var h=document.createElement('div');"
        "h.id='iplug-resize-handle';"
        "h.style.cssText='position:fixed;right:0;bottom:0;width:16px;height:16px;cursor:nwse-resize;z-index:999999;touch-action:none;';"
        "var svg='<svg width=\"16\" height=\"16\" viewBox=\"0 0 16 16\" fill=\"none\" xmlns=\"http://www.w3.org/2000/svg\">"
        "<line x1=\"14\" y1=\"4\" x2=\"4\" y2=\"14\" stroke=\"rgba(255,255,255,0.3)\" stroke-width=\"1.5\"/>"
        "<line x1=\"14\" y1=\"8\" x2=\"8\" y2=\"14\" stroke=\"rgba(255,255,255,0.3)\" stroke-width=\"1.5\"/>"
        "<line x1=\"14\" y1=\"12\" x2=\"12\" y2=\"14\" stroke=\"rgba(255,255,255,0.3)\" stroke-width=\"1.5\"/>"
        "</svg>';"
        "h.innerHTML=svg;"
        "document.body.appendChild(h);"
        "var dw=%d,dh=%d,ratio=dw/dh;"
        "var startX,startY,startW,startH;"
        "h.addEventListener('pointerdown',function(e){"
        "e.preventDefault();e.stopPropagation();"
        "h.setPointerCapture(e.pointerId);"
        "var cs=getComputedStyle(document.documentElement);"
        "var t=cs.transform||cs.webkitTransform||'';"
        "var s=1;var m=t.match(/matrix\\(([^,]+)/);"
        "if(m)s=parseFloat(m[1]);"
        "startW=dw*s;startH=dh*s;"
        "startX=e.clientX*s;startY=e.clientY*s;"
        "});"
        "h.addEventListener('pointermove',function(e){"
        "if(!h.hasPointerCapture(e.pointerId))return;"
        "e.preventDefault();e.stopPropagation();"
        "var cs=getComputedStyle(document.documentElement);"
        "var t=cs.transform||cs.webkitTransform||'';"
        "var s=1;var m=t.match(/matrix\\(([^,]+)/);"
        "if(m)s=parseFloat(m[1]);"
        "var dx=e.clientX*s-startX,dy=e.clientY*s-startY;"
        "var nw=Math.max(200,startW+dx);"
        "var nh=Math.round(nw/ratio);"
        "IPlugSendMsg({msg:'RSZFUI',width:Math.round(nw),height:nh});"
        "});"
        "h.addEventListener('pointerup',function(e){"
        "h.releasePointerCapture(e.pointerId);"
        "});"
        "})();",
        mDesignWidth, mDesignHeight);
      EvaluateJavaScript(resizeJs, nullptr);
    }

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
