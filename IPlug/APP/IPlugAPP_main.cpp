/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

#include <memory>
#include "wdltypes.h"
#include "wdlstring.h"

#include "IPlugPlatform.h"
#include "IPlugAPP_host.h"

#include "config.h"
#include "resource.h"

using namespace iplug;

#pragma mark - WINDOWS
#if defined OS_WIN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <map>
#include <algorithm>

#include "IPlugMidi.h"

// Include stb_image_write for PNG saving
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../Dependencies/IGraphics/STB/stb_image_write.h"

extern WDL_DLGRET MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

HWND gHWND;
extern HINSTANCE gHINSTANCE;
UINT gScrollMessage;

// True when keyboard focus currently sits inside the hosted WebView2. Its host
// window and children all use the "Chrome_WidgetWin" window-class family, so we
// walk up from the focused window looking for one. Used by the message pump to
// bypass IsDialogMessage for the WebView2 — otherwise dialog message processing
// eats the keydown/keyup destined for the web content, and the standalone's
// computer-keyboard → MIDI handler (a document-level listener in the web UI)
// never fires. macOS has no equivalent problem: it feeds the keyboard to MIDI
// natively via SWELLAPP_PROCESSMESSAGE below, not through the webview.
static bool KeyboardFocusIsInWebView()
{
  HWND hFocus = GetFocus();
  for (int depth = 0; hFocus && depth < 8; depth++)
  {
    wchar_t className[64] = {};
    if (GetClassNameW(hFocus, className, 64) > 0 && wcsncmp(className, L"Chrome_WidgetWin", 16) == 0)
      return true;
    hFocus = GetParent(hFocus);
  }
  return false;
}

// Computer-keyboard → MIDI for the Windows standalone player, mirroring the
// macOS SWELLAPP_PROCESSMESSAGE handler below. Home row + upper row play a
// chromatic run (A/S/D/F/G/H/J/K/L white, W/E/T/Y/U/O black, Z/X shift octave).
// Mapped by physical scan code so it's keyboard-layout independent — same layout
// the macOS handler gets from physical virtual-key codes. Unlike macOS (whose
// native SWELL host sees every key event), the shipped web UI has no computer-
// keyboard handler on any platform, so this native path is what makes the
// standalone playable from the QWERTY keys on Windows.
static int IPlugAPPKbdSemitoneWin(unsigned int scanCode)
{
  switch (scanCode)
  {
    case 0x1E: return 0;   // A -> C
    case 0x11: return 1;   // W -> C#
    case 0x1F: return 2;   // S -> D
    case 0x12: return 3;   // E -> D#
    case 0x20: return 4;   // D -> E
    case 0x21: return 5;   // F -> F
    case 0x14: return 6;   // T -> F#
    case 0x22: return 7;   // G -> G
    case 0x15: return 8;   // Y -> G#
    case 0x23: return 9;   // H -> A
    case 0x16: return 10;  // U -> A#
    case 0x24: return 11;  // J -> B
    case 0x25: return 12;  // K -> C
    case 0x18: return 13;  // O -> C#
    case 0x26: return 14;  // L -> D
    default:   return -1;
  }
}

static const unsigned int kIPlugAPPKbdScanZ = 0x2C; // octave down
static const unsigned int kIPlugAPPKbdScanX = 0x2D; // octave up

static int sIPlugAPPKbdOctaveWin = 0;                     // Z/X shift, clamped ±3
static std::map<unsigned int, int> sIPlugAPPKbdHeldWin;   // scanCode → note (dedup + correct note-off)

// Handle a WM_KEYDOWN/WM_KEYUP from the message pump as a computer-keyboard note.
// Returns true when the message was a mapped note/octave key that we consumed —
// the caller then skips dispatch so the key doesn't also reach the WebView2 (the
// macOS handler consumes mapped keys the same way by returning 1). Any other
// message returns false and flows on untouched.
static bool HandleKbdMidiMessage(const MSG& msg)
{
  if (msg.message != WM_KEYDOWN && msg.message != WM_KEYUP)
    return false;

  IPlugAPPHost* pHost = IPlugAPPHost::sInstance.get();
  IPlugAPP* pPlug = pHost ? pHost->GetPlug() : nullptr;
  if (!pPlug)
    return false;

  const unsigned int scanCode = (msg.lParam >> 16) & 0xFF;

  // Note-off is processed regardless of current modifiers: a note started
  // without a modifier must always be released, even if Ctrl/Alt is now held —
  // otherwise the voice sticks.
  if (msg.message == WM_KEYUP)
  {
    auto held = sIPlugAPPKbdHeldWin.find(scanCode);
    if (held == sIPlugAPPKbdHeldWin.end())
      return false;
    IMidiMsg midiMsg; midiMsg.MakeNoteOffMsg(held->second, 0);
    pPlug->SendMidiMsgFromUI(midiMsg);
    sIPlugAPPKbdHeldWin.erase(held);
    return true;
  }

  // WM_KEYDOWN. Skip when Ctrl/Alt is down so app accelerators (screenshot etc.)
  // and system shortcuts keep working.
  if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
    return false;

  const bool isMapped =
    scanCode == kIPlugAPPKbdScanZ || scanCode == kIPlugAPPKbdScanX || IPlugAPPKbdSemitoneWin(scanCode) >= 0;

  // Auto-repeat (lParam bit 30): consume mapped keys without re-triggering, so a
  // held note doesn't leak repeated keydowns to the WebView2. Mirrors the macOS
  // !isARepeat guard.
  if (msg.lParam & 0x40000000)
    return isMapped;

  if (scanCode == kIPlugAPPKbdScanZ) { sIPlugAPPKbdOctaveWin = std::max(-3, sIPlugAPPKbdOctaveWin - 1); return true; }
  if (scanCode == kIPlugAPPKbdScanX) { sIPlugAPPKbdOctaveWin = std::min( 3, sIPlugAPPKbdOctaveWin + 1); return true; }

  const int semitone = IPlugAPPKbdSemitoneWin(scanCode);
  if (semitone >= 0 && sIPlugAPPKbdHeldWin.find(scanCode) == sIPlugAPPKbdHeldWin.end())
  {
    const int note = 48 + sIPlugAPPKbdOctaveWin * 12 + semitone;
    if (note >= 0 && note <= 127)
    {
      sIPlugAPPKbdHeldWin[scanCode] = note;
      IMidiMsg midiMsg; midiMsg.MakeNoteOnMsg(note, 96, 0);
      pPlug->SendMidiMsgFromUI(midiMsg);
    }
    return true;
  }

  return false;
}

// Save a screenshot of the given HWND to a PNG file using Win32 API
bool SaveWindowScreenshot(HWND hwnd, const char* path)
{
  if (!hwnd || !path)
    return false;

  // Get client area dimensions
  RECT clientRect;
  if (!GetClientRect(hwnd, &clientRect))
    return false;

  int width = clientRect.right - clientRect.left;
  int height = clientRect.bottom - clientRect.top;

  if (width <= 0 || height <= 0)
    return false;

  // Create a compatible DC and bitmap
  HDC hdcWindow = GetDC(hwnd);
  if (!hdcWindow)
    return false;

  HDC hdcMem = CreateCompatibleDC(hdcWindow);
  if (!hdcMem)
  {
    ReleaseDC(hwnd, hdcWindow);
    return false;
  }

  // Create a 32-bit DIB section for the screenshot
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height; // Top-down DIB
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* pBits = nullptr;
  HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

  if (!hBitmap || !pBits)
  {
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);
    return false;
  }

  HGDIOBJ hOldBitmap = SelectObject(hdcMem, hBitmap);

  // Use PrintWindow to capture the window content
  // PW_CLIENTONLY captures only the client area
  // PW_RENDERFULLCONTENT renders the window fully, including layered content
#ifndef PW_RENDERFULLCONTENT
  #define PW_RENDERFULLCONTENT 0x00000002
#endif
  BOOL captured = PrintWindow(hwnd, hdcMem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);

  if (!captured)
  {
    // Fallback to BitBlt if PrintWindow fails
    captured = BitBlt(hdcMem, 0, 0, width, height, hdcWindow, 0, 0, SRCCOPY);
  }

  SelectObject(hdcMem, hOldBitmap);
  DeleteDC(hdcMem);
  ReleaseDC(hwnd, hdcWindow);

  if (!captured)
  {
    DeleteObject(hBitmap);
    return false;
  }

  // Convert BGRA to RGBA for stb_image_write
  uint8_t* pixels = static_cast<uint8_t*>(pBits);
  for (int i = 0; i < width * height; i++)
  {
    // Swap B and R channels (BGRA -> RGBA)
    uint8_t temp = pixels[i * 4 + 0];
    pixels[i * 4 + 0] = pixels[i * 4 + 2];
    pixels[i * 4 + 2] = temp;
    // Set alpha to 255 (opaque) since Windows DIB may have garbage in alpha
    pixels[i * 4 + 3] = 255;
  }

  // Use stb_image_write to save as PNG
  int result = stbi_write_png(path, width, height, 4, pixels, width * 4);

  DeleteObject(hBitmap);

  return result != 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdParam, int nShowCmd)
{
  try
  {
#ifndef APP_ALLOW_MULTIPLE_INSTANCES
    HANDLE hMutex = OpenMutex(MUTEX_ALL_ACCESS, 0, BUNDLE_NAME); // BUNDLE_NAME used because it won't have spaces in it
    
    if (!hMutex)
      hMutex = CreateMutex(0, 0, BUNDLE_NAME);
    else
    {
      HWND hWnd = FindWindow(0, BUNDLE_NAME);
      SetForegroundWindow(hWnd);
      return 0;
    }
#endif
    gHINSTANCE = hInstance;
    
    InitCommonControls();
    gScrollMessage = RegisterWindowMessage("MSWHEEL_ROLLMSG");

    IPlugAPPHost* pAppHost = IPlugAPPHost::Create();

    // Parse command line arguments
    if (lpszCmdParam && lpszCmdParam[0])
    {
      char* args = _strdup(lpszCmdParam);
      char* token = strtok(args, " ");
      while (token)
      {
        if (strcmp(token, "--screenshot") == 0)
        {
          token = strtok(nullptr, " ");
          if (token)
            pAppHost->SetScreenshotPath(token);
        }
        else if (strcmp(token, "--no-io") == 0)
        {
          pAppHost->SetNoIO(true);
        }
        token = strtok(nullptr, " ");
      }
      free(args);
    }

    // Screenshot mode implies --no-io
    if (pAppHost->IsScreenshotMode())
      pAppHost->SetNoIO(true);

    pAppHost->Init();
    pAppHost->TryToChangeAudio();

    HACCEL hAccel = LoadAccelerators(gHINSTANCE, MAKEINTRESOURCE(IDR_ACCELERATOR1));

    static UINT(WINAPI *__SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);

    if (!__SetProcessDpiAwarenessContext)
    {
      HINSTANCE h = LoadLibrary("user32.dll");
      if (h) *(void **)&__SetProcessDpiAwarenessContext = GetProcAddress(h, "SetProcessDpiAwarenessContext");
      if (!__SetProcessDpiAwarenessContext)
        *(void **)&__SetProcessDpiAwarenessContext = (void*)(INT_PTR)1;
    }
    if ((UINT_PTR)__SetProcessDpiAwarenessContext > (UINT_PTR)1)
    {
      __SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    CreateDialog(gHINSTANCE, MAKEINTRESOURCE(IDD_DIALOG_MAIN), GetDesktopWindow(), IPlugAPPHost::MainDlgProc);

#if !defined _DEBUG || defined NO_IGRAPHICS
    HMENU menu = GetMenu(gHWND);
    RemoveMenu(menu, 1, MF_BYPOSITION);
    DrawMenuBar(gHWND);
#endif

    for (;;)
    {
      MSG msg= {0,};
      int vvv = GetMessage(&msg, NULL, 0, 0);
      
      if (!vvv)
        break;
      
      if (vvv < 0)
      {
        Sleep(10);
        continue;
      }
      
      if (!msg.hwnd)
      {
        DispatchMessage(&msg);
        continue;
      }

      // Play the standalone from the computer keyboard (mirrors the macOS
      // SWELLAPP_PROCESSMESSAGE handler). Handled here in the pump so it sees the
      // key whether the WebView2 or the dialog has focus; mapped note/octave keys
      // are consumed so they don't also reach the web content. Runs before the
      // accelerator check, but skips itself while Ctrl/Alt is held, so app
      // accelerators (e.g. Ctrl+Shift+S screenshot) are unaffected.
      if (HandleKbdMidiMessage(msg))
        continue;

      // Accelerators run regardless of focus so app shortcuts (screenshot etc.)
      // keep working. This only consumes registered accelerator combos (all of
      // which use Ctrl/Shift), never the plain, unmodified note keys.
      if (gHWND && TranslateAccelerator(gHWND, hAccel, &msg))
        continue;

      // When the WebView2 has keyboard focus, skip all dialog-message
      // processing and let the message dispatch straight to the webview.
      // IsDialogMessage would otherwise intercept keydown/keyup meant for the
      // web content — both the parent dialog (gHWND) and, via the ancestor
      // walk below, any dialog-class parent of the focused webview window — and
      // do dialog Tab-navigation that pulls focus back out. That is exactly
      // what stops the computer keyboard from reaching the QWERTY → MIDI
      // handler on Windows. macOS is unaffected (this whole block is the
      // Windows APP host).
      const bool focusInWebView = KeyboardFocusIsInWebView();

      if (!focusInWebView)
      {
        if (gHWND && IsDialogMessage(gHWND, &msg))
          continue;

        // default processing for other dialogs
        HWND hWndParent = NULL;
        HWND temphwnd = msg.hwnd;

        do
        {
          if (GetClassLong(temphwnd, GCW_ATOM) == (INT)32770)
          {
            hWndParent = temphwnd;
            if (!(GetWindowLong(temphwnd, GWL_STYLE) & WS_CHILD))
              break; // not a child, exit
          }
        }
        while (temphwnd = GetParent(temphwnd));

        if (hWndParent && IsDialogMessage(hWndParent, &msg))
          continue;
      }

      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    
    // in case gHWND didnt get destroyed -- this corresponds to SWELLAPP_DESTROY roughly
    if (gHWND)
      DestroyWindow(gHWND);
    
#ifndef APP_ALLOW_MULTIPLE_INSTANCES
    ReleaseMutex(hMutex);
#endif
  }
  catch(std::exception e)
  {
    DBGMSG("Exception: %s", e.what());
    return 1;
  }
  return 0;
}
#pragma mark - MAC
#elif defined(OS_MAC)
#import <Cocoa/Cocoa.h>
#include <dlfcn.h>
#include <cstring>
#include "IPlugSWELL.h"
#include "IPlugPaths.h"

HWND gHWND;

// Function pointer type for CGWindowListCreateImage
typedef CGImageRef (*CGWindowListCreateImageFunc)(CGRect, uint32_t, uint32_t, uint32_t);

// Save a screenshot of the given HWND (NSView*) to a PNG file
extern "C" bool SaveWindowScreenshot(void* hwnd, const char* path)
{
  if (!hwnd || !path)
    return false;

  NSView* view = (__bridge NSView*)hwnd;
  NSWindow* window = [view window];

  if (!window)
    return false;

  // Get CGWindowListCreateImage via dlsym to bypass availability check
  // The function still exists and works in the runtime
  static CGWindowListCreateImageFunc pCGWindowListCreateImage = nullptr;
  if (!pCGWindowListCreateImage)
  {
    void* handle = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    if (handle)
      pCGWindowListCreateImage = (CGWindowListCreateImageFunc)dlsym(handle, "CGWindowListCreateImage");
  }

  if (!pCGWindowListCreateImage)
    return false;

  // Get the window's CGWindowID
  CGWindowID windowID = (CGWindowID)[window windowNumber];

  // Capture the window content at full resolution (high DPI)
  CGImageRef cgImage = pCGWindowListCreateImage(
    CGRectNull,  // Capture the whole window
    kCGWindowListOptionIncludingWindow,
    windowID,
    kCGWindowImageBoundsIgnoreFraming  // Exclude window frame, capture at screen resolution
  );

  if (!cgImage)
    return false;

  // Create NSBitmapImageRep from CGImage and save as PNG
  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:cgImage];
  CGImageRelease(cgImage);

  if (!bitmap)
    return false;

  NSData* pngData = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  if (!pngData)
    return false;

  NSString* filePath = [NSString stringWithUTF8String:path];
  return [pngData writeToFile:filePath atomically:YES];
}
extern HMENU SWELL_app_stocksysmenu;

static WDL_String gScreenshotPath;
static bool gNoIO = false;

int main(int argc, char *argv[])
{
#if APP_COPY_AUV3
  //if invoked with an argument registerauv3 use plug-in kit to explicitly register auv3 app extension (doesn't happen from debugger)
  if (argc > 2 && std::string_view(argv[2]) == "registerauv3")
  {
    WDL_String appexPath;
    appexPath.SetFormatted(1024, "pluginkit -a %s%s%s.appex", argv[0], "/../../Plugins/", appexPath.get_filepart());
    if (system(appexPath.Get()) > -1)
      NSLog(@"Registered audiounit app extension\n");
    else
      NSLog(@"Failed to register audiounit app extension\n");
  }
#endif

  // Parse command line arguments
  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
    {
      gScreenshotPath.Set(argv[i + 1]);
      i++; // Skip the path argument
    }
    else if (strcmp(argv[i], "--no-io") == 0)
    {
      gNoIO = true;
    }
  }

  if (AppIsSandboxed())
    DBGMSG("App is sandboxed, file system access etc restricted!\n");

  return NSApplicationMain(argc, (const char**) argv);
}

#include "IPlugMidi.h"
#include <map>
#include <algorithm>

// Computer-keyboard → MIDI for the standalone player. Home row + upper row play
// a chromatic run; mapped by physical macOS virtual key code so it's keyboard-
// layout independent (A/S/D/F/G/H/J/K/L white, W/E/T/Y/U/O black, Z/X octave).
// PM addition (vs vanilla iPlug2): lets the standalone be played with no MIDI
// gear, alongside the on-screen keyboard.
static int IPlugAPPKbdSemitone(unsigned short kc)
{
  switch (kc)
  {
    case 0:  return 0;  case 13: return 1;  case 1:  return 2;  case 14: return 3;
    case 2:  return 4;  case 3:  return 5;  case 17: return 6;  case 5:  return 7;
    case 16: return 8;  case 4:  return 9;  case 32: return 10; case 38: return 11;
    case 40: return 12; case 31: return 13; case 37: return 14;
    default: return -1;
  }
}

static int sIPlugAPPKbdOctave = 0;                    // Z/X shift, clamped ±3
static std::map<unsigned short, int> sIPlugAPPKbdHeld; // keyCode → note (dedup + correct note-off)

INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
{
  IPlugAPPHost* pAppHost = nullptr;
  
  switch (msg)
  {
    case SWELLAPP_ONLOAD:
    {
      pAppHost = IPlugAPPHost::Create();

      // Set CLI options
      if (gScreenshotPath.GetLength() > 0)
      {
        pAppHost->SetScreenshotPath(gScreenshotPath.Get());
        pAppHost->SetNoIO(true); // Implicit --no-io for screenshot mode
      }
      else if (gNoIO)
      {
        pAppHost->SetNoIO(true);
      }

      pAppHost->Init();
      pAppHost->TryToChangeAudio();
      break;
    }
    case SWELLAPP_LOADED:
    {
      pAppHost = IPlugAPPHost::sInstance.get();
      
      HMENU menu = SWELL_GetCurrentMenu();
      
      if (menu)
      {
        // work on a new menu
        menu = SWELL_DuplicateMenu(menu);
        HMENU src = LoadMenu(NULL, MAKEINTRESOURCE(IDR_MENU1));

        for (int x = 0; x < GetMenuItemCount(src)-1; x++)
        {
          HMENU sm = GetSubMenu(src,x);
          
          if (sm)
          {
            char str[1024];
            MENUITEMINFO mii = {sizeof(mii), MIIM_TYPE};
            mii.dwTypeData = str;
            mii.cch = sizeof(str);
            str[0] = 0;
            GetMenuItemInfo(src, x, TRUE, &mii);
            MENUITEMINFO mi= {sizeof(mi), MIIM_STATE|MIIM_SUBMENU|MIIM_TYPE,MFT_STRING, 0, 0, SWELL_DuplicateMenu(sm), NULL, NULL, 0, str};
            InsertMenuItem(menu, x+1, TRUE, &mi);
          }
        }
      }
      
      if (menu)
      {
        HMENU sm = GetSubMenu(menu, 1);
        DeleteMenu(sm, ID_QUIT, MF_BYCOMMAND); // remove QUIT from our file menu, since it is in the system menu on OSX
        DeleteMenu(sm, ID_PREFERENCES, MF_BYCOMMAND); // remove PREFERENCES from the file menu, since it is in the system menu on OSX
        
        // remove any trailing separators
        int a = GetMenuItemCount(sm);
        
        while (a > 0 && GetMenuItemID(sm, a-1) == 0)
          DeleteMenu(sm, --a, MF_BYPOSITION);
        
        DeleteMenu(menu, 1, MF_BYPOSITION); // delete file menu
      }
      // Always set up screenshot shortcut
      SetMenuItemModifier(menu, ID_SCREENSHOT, MF_BYCOMMAND, 'S', FCONTROL | FSHIFT);

#if !defined _DEBUG || defined NO_IGRAPHICS
      if (menu)
      {
        HMENU sm = GetSubMenu(menu, 1);
        DeleteMenu(sm, ID_LIVE_EDIT, MF_BYCOMMAND);
        DeleteMenu(sm, ID_SHOW_BOUNDS, MF_BYCOMMAND);
        DeleteMenu(sm, ID_SHOW_DRAWN, MF_BYCOMMAND);
        DeleteMenu(sm, ID_SHOW_FPS, MF_BYCOMMAND);

        // remove any trailing separators
        int a = GetMenuItemCount(sm);

        while (a > 0 && GetMenuItemID(sm, a-1) == 0)
          DeleteMenu(sm, --a, MF_BYPOSITION);

        // Only delete debug menu if it's now empty (screenshot should remain)
        if (GetMenuItemCount(sm) == 0)
          DeleteMenu(menu, 1, MF_BYPOSITION);
      }
#else
      SetMenuItemModifier(menu, ID_LIVE_EDIT, MF_BYCOMMAND, 'E', FCONTROL);
      SetMenuItemModifier(menu, ID_SHOW_DRAWN, MF_BYCOMMAND, 'D', FCONTROL);
      SetMenuItemModifier(menu, ID_SHOW_BOUNDS, MF_BYCOMMAND, 'B', FCONTROL);
      SetMenuItemModifier(menu, ID_SHOW_FPS, MF_BYCOMMAND, 'F', FCONTROL);
#endif

      HWND hwnd = CreateDialog(gHINST, MAKEINTRESOURCE(IDD_DIALOG_MAIN), NULL, IPlugAPPHost::MainDlgProc);
      
      if (menu)
      {
        SetMenu(hwnd, menu); // set the menu for the dialog to our menu (on Windows that menu is set from the .rc, but on SWELL
        SWELL_SetDefaultModalWindowMenu(menu); // other windows will get the stock (bundle) menus
      }
      
      break;
    }
    case SWELLAPP_ONCOMMAND:
      // this is to catch commands coming from the system menu etc
      if (gHWND && (parm1&0xffff))
        SendMessage(gHWND, WM_COMMAND, parm1 & 0xffff, 0);
      break;
    case SWELLAPP_DESTROY:
      if (gHWND)
        DestroyWindow(gHWND);
      break;
    case SWELLAPP_PROCESSMESSAGE:
      MSG* pMSG = (MSG*) parm1;
      NSView* pContentView = (NSView*) pMSG->hwnd;
      NSEvent* pEvent = (NSEvent*) parm2;
      int etype = (int) [pEvent type];
      
      bool textField = [pContentView isKindOfClass:[NSText class]];
      
      if (!textField && etype == NSKeyDown)
      {
        int flag, code = SWELL_MacKeyToWindowsKey(pEvent, &flag);

        if (!(flag&~FVIRTKEY) && (code == VK_RETURN || code == VK_ESCAPE))
        {
          [pContentView keyDown: pEvent];
          return 1;
        }
      }

      // PM: computer-keyboard → MIDI for the standalone player. Skip when a
      // command/control/option modifier is down so app shortcuts still work.
      if (!textField && (etype == NSKeyDown || etype == NSKeyUp))
      {
        IPlugAPPHost* pKbdHost = IPlugAPPHost::sInstance.get();
        IPlugAPP* pKbdPlug = pKbdHost ? pKbdHost->GetPlug() : nullptr;
        const bool modified = ([pEvent modifierFlags] & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) != 0;
        if (pKbdPlug && !modified)
        {
          const unsigned short kc = [pEvent keyCode];
          if (etype == NSKeyDown && ![pEvent isARepeat])
          {
            if (kc == 6)  { sIPlugAPPKbdOctave = std::max(-3, sIPlugAPPKbdOctave - 1); return 1; } // Z
            if (kc == 7)  { sIPlugAPPKbdOctave = std::min( 3, sIPlugAPPKbdOctave + 1); return 1; } // X
            const int st = IPlugAPPKbdSemitone(kc);
            if (st >= 0 && sIPlugAPPKbdHeld.find(kc) == sIPlugAPPKbdHeld.end())
            {
              const int note = 48 + sIPlugAPPKbdOctave * 12 + st;
              if (note >= 0 && note <= 127)
              {
                sIPlugAPPKbdHeld[kc] = note;
                IMidiMsg msg; msg.MakeNoteOnMsg(note, 96, 0);
                pKbdPlug->SendMidiMsgFromUI(msg);
              }
              return 1;
            }
          }
          else if (etype == NSKeyUp)
          {
            auto it = sIPlugAPPKbdHeld.find(kc);
            if (it != sIPlugAPPKbdHeld.end())
            {
              IMidiMsg msg; msg.MakeNoteOffMsg(it->second, 0);
              pKbdPlug->SendMidiMsgFromUI(msg);
              sIPlugAPPKbdHeld.erase(it);
              return 1;
            }
          }
        }
      }
      break;
  }
  return 0;
}

#define CBS_HASSTRINGS 0
#define SWELL_DLG_SCALE_AUTOGEN 1
#define SET_IDD_DIALOG_PREF_SCALE 1.5
#if PLUG_HOST_RESIZE
#define SWELL_DLG_FLAGS_AUTOGEN SWELL_DLG_WS_FLIPPED|SWELL_DLG_WS_RESIZABLE
#endif
#include "swell-dlggen.h"
#include "resources/main.rc_mac_dlg"
#include "swell-menugen.h"
#include "resources/main.rc_mac_menu"

#pragma mark - LINUX
#elif defined(OS_LINUX)
//#include <IPlugSWELL.h>
//#include "swell-internal.h" // fixes problem with HWND forward decl
//
//HWND gHWND;
//UINT gScrollMessage;
//extern HMENU SWELL_app_stocksysmenu;
//
//int main(int argc, char **argv)
//{
//  SWELL_initargs(&argc, &argv);
//  SWELL_Internal_PostMessage_Init();
//  SWELL_ExtendedAPI("APPNAME", (void*) "IGraphics Test");
//
//  HMENU menu = LoadMenu(NULL, MAKEINTRESOURCE(IDR_MENU1));
//  CreateDialog(gHINSTANCE, MAKEINTRESOURCE(IDD_DIALOG_MAIN), NULL, MainDlgProc);
//  SetMenu(gHWND, menu);
//
//  while (!gHWND->m_hashaddestroy)
//  {
//    SWELL_RunMessageLoop();
//    Sleep(10);
//  };
//
//  if (gHWND)
//    DestroyWindow(gHWND);
//
//  return 0;
//}
//
//INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
//{
//  switch (msg)
//  {
//    case SWELLAPP_ONLOAD:
//      break;
//    case SWELLAPP_LOADED:
//    {
//      HMENU menu = SWELL_GetCurrentMenu();
//
//      if (menu)
//      {
//        // work on a new menu
//        menu = SWELL_DuplicateMenu(menu);
//        HMENU src = LoadMenu(NULL, MAKEINTRESOURCE(IDR_MENU1));
//
//        for (auto x = 0; x < GetMenuItemCount(src)-1; x++)
//        {
//          HMENU sm = GetSubMenu(src,x);
//          if (sm)
//          {
//            char str[1024];
//            MENUITEMINFO mii = {sizeof(mii), MIIM_TYPE};
//            mii.dwTypeData = str;
//            mii.cch = sizeof(str);
//            str[0] = 0;
//            GetMenuItemInfo(src, x, TRUE, &mii);
//            MENUITEMINFO mi= {sizeof(mi), MIIM_STATE|MIIM_SUBMENU|MIIM_TYPE,MFT_STRING, 0, 0, SWELL_DuplicateMenu(sm), NULL, NULL, 0, str};
//            InsertMenuItem(menu, x+1, TRUE, &mi);
//          }
//        }
//      }
//
//      if (menu)
//      {
//        HMENU sm = GetSubMenu(menu, 1);
//        DeleteMenu(sm, ID_QUIT, MF_BYCOMMAND); // remove QUIT from our file menu, since it is in the system menu on OSX
//        DeleteMenu(sm, ID_PREFERENCES, MF_BYCOMMAND); // remove PREFERENCES from the file menu, since it is in the system menu on OSX
//
//        // remove any trailing separators
//        int a = GetMenuItemCount(sm);
//
//        while (a > 0 && GetMenuItemID(sm, a-1) == 0)
//          DeleteMenu(sm, --a, MF_BYPOSITION);
//
//        DeleteMenu(menu, 1, MF_BYPOSITION); // delete file menu
//      }
//
//      // if we want to set any default modifiers for items in the menus, we can use:
//      // SetMenuItemModifier(menu,commandID,MF_BYCOMMAND,'A',FCONTROL) etc.
//
//      HWND hwnd = CreateDialog(gHINST,MAKEINTRESOURCE(IDD_DIALOG_MAIN), NULL, MainDlgProc);
//
//      if (menu)
//      {
//        SetMenu(hwnd, menu); // set the menu for the dialog to our menu (on Windows that menu is set from the .rc, but on SWELL
//        SWELL_SetDefaultModalWindowMenu(menu); // other windows will get the stock (bundle) menus
//      }
//
//      break;
//    }
//    case SWELLAPP_ONCOMMAND:
//      // this is to catch commands coming from the system menu etc
//      if (gHWND && (parm1&0xffff))
//        SendMessage(gHWND, WM_COMMAND, parm1 & 0xffff, 0);
//      break;
//    case SWELLAPP_DESTROY:
//      if (gHWND)
//        DestroyWindow(gHWND);
//      break;
//    case SWELLAPP_PROCESSMESSAGE: // can hook keyboard input here
//      // parm1 = (MSG*), should we want it -- look in swell.h to see what the return values refer to
//      break;
//  }
//  return 0;
//}
//
//#define CBS_HASSTRINGS 0
//#define SWELL_DLG_SCALE_AUTOGEN 1
//#define SET_IDD_DIALOG_PREF_SCALE 1.5
//#include "swell-dlggen.h"
//#include "resources/main.rc_mac_dlg"
//#include "swell-menugen.h"
//#include "resources/main.rc_mac_menu"
#endif
