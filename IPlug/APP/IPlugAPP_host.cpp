/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

#include "IPlugAPP_host.h"

#ifdef OS_WIN
#include <sys/stat.h>
#include "win32_utf8.h"
#endif

#include "IPlugLogger.h"
#include "../../Dependencies/Extras/nlohmann/json.hpp"

using namespace iplug;

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 2048
#endif

#define STRBUFSZ 100

std::unique_ptr<IPlugAPPHost> IPlugAPPHost::sInstance;
UINT gSCROLLMSG;

IPlugAPPHost::IPlugAPPHost()
: mIPlug(MakePlug(InstanceInfo{this}))
{
}

IPlugAPPHost::~IPlugAPPHost()
{
  mExiting = true;
  
  CloseAudio();
  
  if (mMidiIn)
    mMidiIn->cancelCallback();

  if (mMidiOut)
    mMidiOut->closePort();
}

//static
IPlugAPPHost* IPlugAPPHost::Create()
{
  sInstance = std::make_unique<IPlugAPPHost>();
  return sInstance.get();
}

bool IPlugAPPHost::Init()
{
  mIPlug->SetHost("standalone", mIPlug->GetPluginVersion(false));
    
  if (!InitState())
    return false;
  
  TryToChangeAudioDriverType(); // will init RTAudio with an API type based on gState->mAudioDriverType
  ProbeAudioIO(); // find out what audio IO devs are available and put their IDs in the global variables gAudioInputDevs / gAudioOutputDevs
  InitMidi(); // creates RTMidiIn and RTMidiOut objects
  ProbeMidiIO(); // find out what midi IO devs are available and put their names in the global variables gMidiInputDevs / gMidiOutputDevs
  SelectMIDIDevice(ERoute::kInput, mState.mMidiInDev.Get());
  SelectMIDIDevice(ERoute::kOutput, mState.mMidiOutDev.Get());
  
  mIPlug->OnParamReset(kReset);
  mIPlug->OnActivate(true);
  
  return true;
}

bool IPlugAPPHost::OpenWindow(HWND pParent)
{
  return mIPlug->OpenWindow(pParent) != nullptr;
}

void IPlugAPPHost::CloseWindow()
{
  mIPlug->CloseWindow();
}

bool IPlugAPPHost::InitState()
{
#if defined OS_WIN
  char strPath[MAX_PATH_LEN];
  SHGetSpecialFolderPathUTF8(NULL, strPath, MAX_PATH_LEN, CSIDL_LOCAL_APPDATA, FALSE);
  mINIPath.SetFormatted(MAX_PATH_LEN, "%s\\%s\\", strPath, BUNDLE_NAME);
#elif defined OS_MAC
  mINIPath.SetFormatted(MAX_PATH_LEN, "%s/Library/Application Support/%s/", getenv("HOME"), BUNDLE_NAME);
#else
  #error NOT IMPLEMENTED
#endif

  struct stat st;

  if (stat(mINIPath.Get(), &st) == 0) // if directory exists
  {
    mINIPath.Append("settings.ini"); // add file name to path

    char buf[STRBUFSZ];
    
    if (stat(mINIPath.Get(), &st) == 0) // if settings file exists read values into state
    {
      DBGMSG("Reading ini file from %s\n", mINIPath.Get());
      
      mState.mAudioDriverType = GetPrivateProfileInt("audio", "driver", 0, mINIPath.Get());

      GetPrivateProfileString("audio", "indev", "Built-in Input", buf, STRBUFSZ, mINIPath.Get()); mState.mAudioInDev.Set(buf);
      GetPrivateProfileString("audio", "outdev", "Built-in Output", buf, STRBUFSZ, mINIPath.Get()); mState.mAudioOutDev.Set(buf);

      //audio
      mState.mAudioInChanL = GetPrivateProfileInt("audio", "in1", 1, mINIPath.Get()); // 1 is first audio input
      mState.mAudioInChanR = GetPrivateProfileInt("audio", "in2", 2, mINIPath.Get());
      mState.mAudioOutChanL = GetPrivateProfileInt("audio", "out1", 1, mINIPath.Get()); // 1 is first audio output
      mState.mAudioOutChanR = GetPrivateProfileInt("audio", "out2", 2, mINIPath.Get());
      //mState.mAudioInIsMono = GetPrivateProfileInt("audio", "monoinput", 0, mINIPath.Get());

      mState.mBufferSize = GetPrivateProfileInt("audio", "buffer", 512, mINIPath.Get());
      mState.mAudioSR = GetPrivateProfileInt("audio", "sr", 44100, mINIPath.Get());

      //midi
      GetPrivateProfileString("midi", "indev", "no input", buf, STRBUFSZ, mINIPath.Get()); mState.mMidiInDev.Set(buf);
      GetPrivateProfileString("midi", "outdev", "no output", buf, STRBUFSZ, mINIPath.Get()); mState.mMidiOutDev.Set(buf);

      mState.mMidiInChan = GetPrivateProfileInt("midi", "inchan", 0, mINIPath.Get()); // 0 is any
      mState.mMidiOutChan = GetPrivateProfileInt("midi", "outchan", 0, mINIPath.Get()); // 1 is first chan
    }

    // if settings file doesn't exist, populate with default values, otherwise overwrite
    UpdateINI();
  }
  else // folder doesn't exist - make folder and make file
  {
#if defined OS_WIN
    // folder doesn't exist - make folder and make file
    CreateDirectory(mINIPath.Get(), NULL);
    mINIPath.Append("settings.ini");
    UpdateINI(); // will write file if doesn't exist
#elif defined OS_MAC
    mode_t process_mask = umask(0);
    int result_code = mkdir(mINIPath.Get(), S_IRWXU | S_IRWXG | S_IRWXO);
    umask(process_mask);

    if (!result_code)
    {
      mINIPath.Append("settings.ini");
      UpdateINI(); // will write file if doesn't exist
    }
    else
    {
      return false;
    }
#else
  #error NOT IMPLEMENTED
#endif
  }

  return true;
}

void IPlugAPPHost::UpdateINI()
{
  char buf[STRBUFSZ]; // temp buffer for writing integers to profile strings
  const char* ini = mINIPath.Get();

  sprintf(buf, "%u", mState.mAudioDriverType);
  WritePrivateProfileString("audio", "driver", buf, ini);

  WritePrivateProfileString("audio", "indev", mState.mAudioInDev.Get(), ini);
  WritePrivateProfileString("audio", "outdev", mState.mAudioOutDev.Get(), ini);

  sprintf(buf, "%u", mState.mAudioInChanL);
  WritePrivateProfileString("audio", "in1", buf, ini);
  sprintf(buf, "%u", mState.mAudioInChanR);
  WritePrivateProfileString("audio", "in2", buf, ini);
  sprintf(buf, "%u", mState.mAudioOutChanL);
  WritePrivateProfileString("audio", "out1", buf, ini);
  sprintf(buf, "%u", mState.mAudioOutChanR);
  WritePrivateProfileString("audio", "out2", buf, ini);
  //sprintf(buf, "%u", mState.mAudioInIsMono);
  //WritePrivateProfileString("audio", "monoinput", buf, ini);

  WDL_String str;
  str.SetFormatted(32, "%i", mState.mBufferSize);
  WritePrivateProfileString("audio", "buffer", str.Get(), ini);

  str.SetFormatted(32, "%i", mState.mAudioSR);
  WritePrivateProfileString("audio", "sr", str.Get(), ini);

  WritePrivateProfileString("midi", "indev", mState.mMidiInDev.Get(), ini);
  WritePrivateProfileString("midi", "outdev", mState.mMidiOutDev.Get(), ini);

  sprintf(buf, "%u", mState.mMidiInChan);
  WritePrivateProfileString("midi", "inchan", buf, ini);
  sprintf(buf, "%u", mState.mMidiOutChan);
  WritePrivateProfileString("midi", "outchan", buf, ini);
}

std::string IPlugAPPHost::GetAudioDeviceName(uint32_t deviceID) const
{
  auto str = mDAC->getDeviceInfo(deviceID).name;
  std::size_t pos = str.find(':');

  if (pos != std::string::npos)
  {
    std::string subStr = str.substr(pos + 1);
    // Strip the leading space left by "Manufacturer: Device" splitting. The
    // INI parser (GetPrivateProfileString) strips leading whitespace on read,
    // so a space-prefixed name saved to settings.ini never exact-matched on
    // the next launch and the app silently fell back to the default device.
    // Trimming here keeps every consumer (INI round-trip, webview picker,
    // CoreAudio matching) consistent.
    while (!subStr.empty() && subStr.front() == ' ')
      subStr.erase(subStr.begin());
    return subStr;
  }
  else
  {
    return str;
  }
}

std::optional<uint32_t> IPlugAPPHost::GetAudioDeviceID(const char* deviceNameToTest) const
{
  auto deviceIDs = mDAC->getDeviceIds();

  for (auto deviceID : deviceIDs)
  {
    auto name = GetAudioDeviceName(deviceID);

    if (std::string_view(deviceNameToTest) == name)
    {
      return deviceID;
    }
  }

  return std::nullopt;
}

std::optional<uint32_t> IPlugAPPHost::GetAudioOutputDeviceID(const char* deviceNameToTest) const
{
  std::optional<uint32_t> best;
  unsigned int bestChannels = 0;

  // Match only output-capable devices, and among same-named ones keep the
  // variant with the most output channels — Bluetooth headsets (AirPods)
  // surface a mono HFP endpoint and a stereo A2DP output under one name, and
  // binding the output stream to the mono one makes openStream(2ch) fail.
  for (auto deviceID : mAudioOutputDevIDs)
  {
    if (std::string_view(deviceNameToTest) != GetAudioDeviceName(deviceID))
      continue;

    const unsigned int channels = mDAC->getDeviceInfo(deviceID).outputChannels;

    if (!best || channels > bestChannels)
    {
      best = deviceID;
      bestChannels = channels;
    }
  }

  return best;
}

int IPlugAPPHost::GetMIDIPortNumber(ERoute direction, const char* nameToTest) const
{
  int start = 1;
  
  auto nameStrView = std::string_view(nameToTest);
  
  if (direction == ERoute::kInput)
  {
    if (nameStrView == OFF_TEXT) return 0;
    
  #ifdef OS_MAC
    start = 2;
    if (nameStrView == "virtual input") return 1;
  #endif
    
    for (int i = 0; i < mMidiIn->getPortCount(); i++)
    {
      if (nameStrView == mMidiIn->getPortName(i).c_str())
        return (i + start);
    }
  }
  else
  {
    if (nameStrView == OFF_TEXT) return 0;
  
  #ifdef OS_MAC
    start = 2;
    if (nameStrView == "virtual output") return 1;
  #endif
  
    for (int i = 0; i < mMidiOut->getPortCount(); i++)
    {
      if (nameStrView == mMidiOut->getPortName(i).c_str())
        return (i + start);
    }
  }
  
  return -1;
}

void IPlugAPPHost::ProbeAudioIO()
{
  mAudioInputDevIDs.clear();
  mAudioOutputDevIDs.clear();

  if (!mDAC)
    return;

  DBGMSG("\nRtAudio Version %s", RtAudio::getVersion().c_str());

  RtAudio::DeviceInfo info;

  auto deviceIDs = mDAC->getDeviceIds();

  for (auto deviceID : deviceIDs)
  {
    info = mDAC->getDeviceInfo(deviceID);

    if (info.inputChannels > 0)
    {
      mAudioInputDevIDs.push_back(deviceID);
    }
    
    if (info.outputChannels > 0)
    {
      mAudioOutputDevIDs.push_back(deviceID);
    }
    
    if (info.isDefaultInput)
    {
      mDefaultInputDev = deviceID;
    }
    
    if (info.isDefaultOutput)
    {
      mDefaultOutputDev = deviceID;
    }
  }
}

void IPlugAPPHost::ProbeMidiIO()
{
  if (!mMidiIn || !mMidiOut)
    return;
  else
  {
    int nInputPorts = mMidiIn->getPortCount();

    mMidiInputDevNames.push_back(OFF_TEXT);

#ifdef OS_MAC
    mMidiInputDevNames.push_back("virtual input");
#endif

    for (int i=0; i<nInputPorts; i++)
    {
      mMidiInputDevNames.push_back(mMidiIn->getPortName(i));
    }

    int nOutputPorts = mMidiOut->getPortCount();

    mMidiOutputDevNames.push_back(OFF_TEXT);

#ifdef OS_MAC
    mMidiOutputDevNames.push_back("virtual output");
#endif

    for (int i=0; i<nOutputPorts; i++)
    {
      mMidiOutputDevNames.push_back(mMidiOut->getPortName(i));
      //This means the virtual output port wont be added as an input
    }
  }
}

bool IPlugAPPHost::AudioSettingsInStateAreEqual(AppState& os, AppState& ns)
{
  if (os.mAudioDriverType != ns.mAudioDriverType) return false;
  if (std::string_view(os.mAudioInDev.Get()) != ns.mAudioInDev.Get()) return false;
  if (std::string_view(os.mAudioOutDev.Get()) != ns.mAudioOutDev.Get()) return false;
  if (os.mAudioSR != ns.mAudioSR) return false;
  if (os.mBufferSize != ns.mBufferSize) return false;
  if (os.mAudioInChanL != ns.mAudioInChanL) return false;
  if (os.mAudioInChanR != ns.mAudioInChanR) return false;
  if (os.mAudioOutChanL != ns.mAudioOutChanL) return false;
  if (os.mAudioOutChanR != ns.mAudioOutChanR) return false;
//  if (os.mAudioInIsMono != ns.mAudioInIsMono) return false;

  return true;
}

bool IPlugAPPHost::MIDISettingsInStateAreEqual(AppState& os, AppState& ns)
{
  if (std::string_view(os.mMidiInDev.Get()) != ns.mMidiInDev.Get()) return false;
  if (std::string_view(os.mMidiOutDev.Get()) != ns.mMidiOutDev.Get()) return false;
  if (os.mMidiInChan != ns.mMidiInChan) return false;
  if (os.mMidiOutChan != ns.mMidiOutChan) return false;

  return true;
}

bool IPlugAPPHost::TryToChangeAudioDriverType()
{
  CloseAudio();

  if (mDAC)
  {
    mDAC = nullptr;
  }

  // Skip RtAudio initialization in no-I/O mode or screenshot mode
  if (mNoIO || IsScreenshotMode())
    return true;

#if defined OS_WIN
  if (mState.mAudioDriverType == kDeviceASIO)
    mDAC = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
  else if (mState.mAudioDriverType == kDeviceDS)
    mDAC = std::make_unique<RtAudio>(RtAudio::WINDOWS_DS);
#elif defined OS_MAC
  if (mState.mAudioDriverType == kDeviceCoreAudio)
    mDAC = std::make_unique<RtAudio>(RtAudio::MACOSX_CORE);
  //else
  //mDAC = std::make_unique<RtAudio>(RtAudio::UNIX_JACK);
#else
  #error NOT IMPLEMENTED
#endif

  if (mDAC)
  {
    mDAC->setErrorCallback(ErrorCallback);
    return true;
  }

  return false;
}

bool IPlugAPPHost::TryToChangeAudio()
{
  // Skip audio initialization in no-I/O mode or screenshot mode
  if (mNoIO || IsScreenshotMode())
    return true;

#if defined OS_WIN
  // ASIO has one device, use the output for the input ID
  auto inputID = GetAudioDeviceID(mState.mAudioDriverType == kDeviceASIO ? mState.mAudioOutDev.Get() : mState.mAudioInDev.Get());
#elif defined OS_MAC
  auto inputID = GetAudioDeviceID(mState.mAudioInDev.Get());
#else
  #error NOT IMPLEMENTED
#endif
  auto outputID = GetAudioOutputDeviceID(mState.mAudioOutDev.Get());

  bool failedToFindDevice = false;
  bool resetToDefault = false;

  if (!inputID)
  {
    if (mDefaultInputDev)
    {
      resetToDefault = true;
      inputID = mDefaultInputDev;

      if (mAudioInputDevIDs.size())
        mState.mAudioInDev.Set(GetAudioDeviceName(inputID.value()).c_str());
    }
    else
      failedToFindDevice = true;
  }

  if (!outputID)
  {
    if (mDefaultOutputDev)
    {
      resetToDefault = true;
      outputID = mDefaultOutputDev;

      if (mAudioOutputDevIDs.size())
        mState.mAudioOutDev.Set(GetAudioDeviceName(outputID.value()).c_str());
    }
    else
      failedToFindDevice = true;
  }

  if (resetToDefault)
  {
    DBGMSG("Couldn't find previous audio device, reseting to default\n");
    UpdateINI();
  }

  if (failedToFindDevice)
    MessageBox(gHWND, "Please check the audio settings", "Error", MB_OK);

  if (inputID && outputID)
  {
    return InitAudio(inputID.value(), outputID.value(), mState.mAudioSR, mState.mBufferSize);
  }

  return false;
}

bool IPlugAPPHost::SelectMIDIDevice(ERoute direction, const char* pPortName)
{
  int port = GetMIDIPortNumber(direction, pPortName);

  if (direction == ERoute::kInput)
  {
    if (port == -1)
    {
      mState.mMidiInDev.Set(OFF_TEXT);
      UpdateINI();
      port = 0;
    }

    //TODO: send all notes off?
    if (mMidiIn)
    {
      mMidiIn->closePort();

      if (port == 0)
      {
        return true;
      }
  #if defined OS_WIN
      else
      {
        mMidiIn->openPort(port-1);
        return true;
      }
  #elif defined OS_MAC
      else if (port == 1)
      {
        std::string virtualMidiInputName = "To ";
        virtualMidiInputName += BUNDLE_NAME;
        mMidiIn->openVirtualPort(virtualMidiInputName);
        return true;
      }
      else
      {
        mMidiIn->openPort(port-2);
        return true;
      }
  #else
   #error NOT IMPLEMENTED
  #endif
    }
  }
  else
  {
    if (port == -1)
    {
      mState.mMidiOutDev.Set(OFF_TEXT);
      UpdateINI();
      port = 0;
    }
    
    if (mMidiOut)
    {
      //TODO: send all notes off?
      mMidiOut->closePort();
      
      if (port == 0)
        return true;
#if defined OS_WIN
      else
      {
        mMidiOut->openPort(port-1);
        return true;
      }
#elif defined OS_MAC
      else if (port == 1)
      {
        std::string virtualMidiOutputName = "From ";
        virtualMidiOutputName += BUNDLE_NAME;
        mMidiOut->openVirtualPort(virtualMidiOutputName);
        return true;
      }
      else
      {
        mMidiOut->openPort(port-2);
        return true;
      }
#else
  #error NOT IMPLEMENTED
#endif
    }
  }
  
  return false;
}

std::vector<std::string> IPlugAPPHost::GetAudioOutputDeviceNames() const
{
  std::vector<std::string> names;
  names.reserve(mAudioOutputDevIDs.size());
  for (auto id : mAudioOutputDevIDs)
    names.push_back(GetAudioDeviceName(id));
  return names;
}

std::vector<std::string> IPlugAPPHost::GetMIDIInputDeviceNames() const
{
  return mMidiInputDevNames;
}

bool IPlugAPPHost::SelectAudioOutDevice(const char* name)
{
  if (!name)
    return false;

  mState.mAudioOutDev.Set(name);
  const bool changed = TryToChangeAudio();
  UpdateINI();
  return changed;
}

// Bridge helpers for the standalone's webview device pickers. Declared in
// IPlugWebViewEditorDelegate.h (APP target only) so the generic WebView header
// never pulls in RtAudio/RtMidi/IPlugAPPHost. They reach the single app-host
// instance, marshal the device lists across to the webview, and apply a
// selection coming back from the picker UI.
namespace iplug {

std::string PMGetDeviceListScript()
{
  nlohmann::json payload;
  IPlugAPPHost* pHost = IPlugAPPHost::sInstance.get();

  if (pHost)
  {
    payload["audioOut"] = pHost->GetAudioOutputDeviceNames();
    payload["midiIn"] = pHost->GetMIDIInputDeviceNames();
    payload["selAudioOut"] = pHost->GetSelectedAudioOutDeviceName();
    payload["selMidiIn"] = pHost->GetSelectedMIDIInDeviceName();
  }
  else
  {
    payload["audioOut"] = std::vector<std::string>{};
    payload["midiIn"] = std::vector<std::string>{};
    payload["selAudioOut"] = "";
    payload["selMidiIn"] = "";
  }

  return "if(window.PMOnDevices)window.PMOnDevices(" + payload.dump() + ");";
}

void PMSelectAudioOutputDevice(const char* name)
{
  IPlugAPPHost* pHost = IPlugAPPHost::sInstance.get();
  if (pHost && name)
    pHost->SelectAudioOutDevice(name);
}

void PMSelectMIDIInputDevice(const char* name)
{
  IPlugAPPHost* pHost = IPlugAPPHost::sInstance.get();
  if (pHost && name)
    pHost->SelectMIDIDevice(ERoute::kInput, name);
}

} // namespace iplug

void IPlugAPPHost::CloseAudio()
{
  if (mDAC && mDAC->isStreamOpen())
  {
    if (mDAC->isStreamRunning())
    {
      mAudioEnding = true;
    
      while (!mAudioDone)
        Sleep(10);
      
      mDAC->abortStream();
    }
    
    mDAC->closeStream();
  }
}

#if defined OS_MAC
#include <CoreAudio/CoreAudio.h>

// Silent render callback for the Bluetooth wake pre-roll below — pumps zeros.
// userData points at the stream's channel count (RTAUDIO_FLOAT64, contiguous
// either way under RTAUDIO_NONINTERLEAVED, so one memset covers the buffer).
static int PMSilentWakeCallback(void* pOutputBuffer, void*, unsigned int nFrames, double, RtAudioStreamStatus, void* pUserData)
{
  unsigned int nChans = *static_cast<unsigned int*>(pUserData);
  memset(pOutputBuffer, 0, nFrames * nChans * sizeof(double));
  return 0;
}

// Minimal CoreAudio helpers for the Bluetooth handling: RtAudio's device ids
// are its own (and renumber on BT reconnect), so the route-state check and the
// nominal-rate query talk to CoreAudio directly, matching the device by name.

static std::string PMCAGetDeviceName(AudioDeviceID device)
{
  CFStringRef name = nullptr;
  UInt32 size = sizeof(name);
  AudioObjectPropertyAddress address = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) != noErr || !name)
    return "";
  char buf[256] = {0};
  CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
  CFRelease(name);
  return buf;
}

static unsigned int PMCAOutputChannels(AudioDeviceID device)
{
  AudioObjectPropertyAddress address = { kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain };
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0)
    return 0;
  std::vector<char> raw(size);
  AudioBufferList* pBufferList = (AudioBufferList*) raw.data();
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, pBufferList) != noErr)
    return 0;
  unsigned int total = 0;
  for (UInt32 i = 0; i < pBufferList->mNumberBuffers; i++)
    total += pBufferList->mBuffers[i].mNumberChannels;
  return total;
}

// Match by name among output-capable devices, preferring the most output
// channels — mirrors GetAudioOutputDeviceID's disambiguation of same-named
// Bluetooth endpoints (mono HFP vs stereo A2DP).
static AudioDeviceID PMCAFindOutputByName(const std::string& wanted)
{
  AudioObjectPropertyAddress address = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
    return kAudioObjectUnknown;
  std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr)
    return kAudioObjectUnknown;

  AudioDeviceID best = kAudioObjectUnknown;
  unsigned int bestChannels = 0;
  for (auto device : devices)
  {
    unsigned int channels = PMCAOutputChannels(device);
    if (channels == 0 || PMCAGetDeviceName(device) != wanted)
      continue;
    if (best == kAudioObjectUnknown || channels > bestChannels)
    {
      best = device;
      bestChannels = channels;
    }
  }
  return best;
}

static bool PMCAIsRunningSomewhere(AudioDeviceID device)
{
  AudioObjectPropertyAddress address = { kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  UInt32 running = 0, size = sizeof(running);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &running) != noErr)
    return false;
  return running != 0;
}

static Float64 PMCAGetNominalRate(AudioDeviceID device)
{
  AudioObjectPropertyAddress address = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  Float64 rate = 0;
  UInt32 size = sizeof(rate);
  AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate);
  return rate;
}
#endif

bool IPlugAPPHost::InitAudio(uint32_t inID, uint32_t outID, uint32_t sr, uint32_t iovs)
{
  CloseAudio();

  RtAudio::StreamParameters iParams, oParams;
  iParams.deviceId = inID;
  iParams.nChannels = GetPlug()->MaxNChannels(ERoute::kInput); // TODO: flexible channel count
  iParams.firstChannel = 0; // TODO: flexible channel count

  oParams.deviceId = outID;
  oParams.nChannels = GetPlug()->MaxNChannels(ERoute::kOutput); // TODO: flexible channel count
  oParams.firstChannel = 0; // TODO: flexible channel count

#if defined OS_MAC
  // Bluetooth headsets (AirPods): NEVER set/flip the device's nominal sample
  // rate — follow it. RtAudio silently sets the nominal rate to whatever we
  // request; on Bluetooth that clock yank (44.1k <-> 24k <-> 48k as the
  // headset renegotiates profiles) can wedge the entire route at the OS level:
  // audio dies for EVERY app (even Apple's `say`) until the headset
  // reconnects. Instead, open at the device's CURRENT nominal rate and run the
  // DSP there — the open is then a no-op rate-wise (fast, no 2s rate-update
  // waits, no wedge risk). 24000 in the output device's rate list is the
  // Bluetooth-telephony tell; built-in/USB/virtual devices don't expose it
  // (and RtAudio's DirectSound backend lists 24k for most devices — hence the
  // OS_MAC gate). Pro-audio interfaces are untouched: without the 24k tell we
  // honor the user's configured rate as before.
  {
    auto outInfo = mDAC->getDeviceInfo(outID);
    bool btTelephonyCapable = false;
    for (auto rate : outInfo.sampleRates)
    {
      if (rate == 24000) { btTelephonyCapable = true; break; }
    }

    const std::string outName = GetAudioDeviceName(outID);
    AudioDeviceID caDevice = btTelephonyCapable ? PMCAFindOutputByName(outName) : kAudioObjectUnknown;

    if (btTelephonyCapable && caDevice != kAudioObjectUnknown)
    {
      const Float64 nominal = PMCAGetNominalRate(caDevice);
      if (nominal >= 8000.0 && (uint32_t) nominal != sr)
      {
        std::cerr << "BT follow-device: opening at nominal " << nominal << " instead of requested " << sr << std::endl;
        sr = (uint32_t) nominal;
      }

      // If nothing is rendering to the device, feed it ~300ms of silence at
      // ITS OWN rate before the real open (no clock change involved) so a
      // parked route is live when the real stream starts. Skipped whenever any
      // client (music app, previous stream) already runs the device.
      if (!PMCAIsRunningSomewhere(caDevice))
      {
        std::cerr << "BT wake: '" << outName << "' idle — pre-rolling silence at " << sr << std::endl;
        RtAudio::StreamParameters wakeParams;
        wakeParams.deviceId = outID;
        wakeParams.nChannels = oParams.nChannels;
        wakeParams.firstChannel = 0;
        RtAudio::StreamOptions wakeOptions;
        wakeOptions.flags = RTAUDIO_NONINTERLEAVED;
        unsigned int wakeFrames = 512;
        unsigned int wakeChans = wakeParams.nChannels;

        if (mDAC->openStream(&wakeParams, nullptr, RTAUDIO_FLOAT64, sr, &wakeFrames, &PMSilentWakeCallback, &wakeChans, &wakeOptions) == RTAUDIO_NO_ERROR)
        {
          mDAC->startStream();
          Sleep(300);
          mDAC->stopStream();
          mDAC->closeStream();
        }
        else
        {
          std::cerr << "BT wake: pre-roll open failed: " << mDAC->getErrorText() << std::endl;
        }
      }
    }
  }
#endif

  mBufferSize = iovs; // mBufferSize may get changed by stream

  DBGMSG("trying to start audio stream @ %i sr, buffer size %i\nindev = %s\noutdev = %s\ninputs = %i\noutputs = %i\n",
    sr, mBufferSize, GetAudioDeviceName(inID).c_str(), GetAudioDeviceName(outID).c_str(), iParams.nChannels, oParams.nChannels);

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_NONINTERLEAVED;
  // options.streamName = BUNDLE_NAME; // JACK stream name, not used on other streams

  mBufIndex = 0;
  mSamplesElapsed = 0;
  mSampleRate = static_cast<double>(sr);
  mVecWait = 0;
  mAudioEnding = false;
  mAudioDone = false;
  
  mIPlug->SetBlockSize(APP_SIGNAL_VECTOR_SIZE);
  mIPlug->SetSampleRate(mSampleRate);
  mIPlug->OnReset();

  auto status = mDAC->openStream(&oParams, iParams.nChannels > 0 ? &iParams : nullptr, RTAUDIO_FLOAT64, sr, &mBufferSize, &AudioCallback, this, &options);

#if defined OS_MAC
  // Bluetooth devices change their nominal sample rate asynchronously as the
  // headset renegotiates profiles (A2DP <-> HFP). RtAudio's probeDeviceOpen
  // only waits 2 s for the rate update and fails with "timeout waiting for
  // sample rate update" when the transport is still settling — which would
  // leave the app with NO audio stream at all (silent output, frozen meters).
  // The follow-device logic above makes this rare (we open at the current
  // nominal rate), but a profile transition can still race the open; each
  // retry waits another window so slow transitions settle instead of failing
  // audio init outright.
  for (int attempt = 0; status != RtAudioErrorType::RTAUDIO_NO_ERROR && attempt < 2; attempt++)
  {
    DBGMSG("openStream failed (%s) — retrying after settle\n", mDAC->getErrorText().c_str());
    Sleep(500);
    status = mDAC->openStream(&oParams, iParams.nChannels > 0 ? &iParams : nullptr, RTAUDIO_FLOAT64, sr, &mBufferSize, &AudioCallback, this, &options);
  }
#endif

  if (status != RtAudioErrorType::RTAUDIO_NO_ERROR)
  {
    DBGMSG("%s", mDAC->getErrorText().c_str());
    return false;
  }

  for (int i = 0; i < iParams.nChannels; i++)
  {
    mInputBufPtrs.Add(nullptr); //will be set in callback
  }
    
  for (int i = 0; i < oParams.nChannels; i++)
  {
    mOutputBufPtrs.Add(nullptr); //will be set in callback
  }
    
  if (mDAC->startStream() != RTAUDIO_NO_ERROR)
  {
    DBGMSG("Error starting stream: %s\n", mDAC->getErrorText().c_str());
    return false;
  }

  mActiveState = mState;

  return true;
}

bool IPlugAPPHost::InitMidi()
{
  // Skip MIDI initialization in no-I/O mode or screenshot mode
  if (mNoIO || IsScreenshotMode())
    return true;

  try
  {
    mMidiIn = std::make_unique<RtMidiIn>();
  }
  catch (RtMidiError &error)
  {
    mMidiIn = nullptr;
    error.printMessage();
    return false;
  }

  try
  {
    mMidiOut = std::make_unique<RtMidiOut>();
  }
  catch (RtMidiError &error)
  {
    mMidiOut = nullptr;
    error.printMessage();
    return false;
  }

  mMidiIn->setCallback(&MIDICallback, this);
  mMidiIn->ignoreTypes(false, true, false );

  return true;
}

void ApplyFades(double *pBuffer, int nChans, int nFrames, bool down)
{
  for (int i = 0; i < nChans; i++)
  {
    double *pIO = pBuffer + (i * nFrames);
    
    if (down)
    {
      for (int j = 0; j < nFrames; j++)
        pIO[j] *= ((double) (nFrames - (j + 1)) / (double) nFrames);
    }
    else
    {
      for (int j = 0; j < nFrames; j++)
        pIO[j] *= ((double) j / (double) nFrames);
    }
  }
}

// static
int IPlugAPPHost::AudioCallback(void* pOutputBuffer, void* pInputBuffer, uint32_t nFrames, double streamTime, RtAudioStreamStatus status, void* pUserData)
{
  IPlugAPPHost* _this = (IPlugAPPHost*) pUserData;

  int nins = _this->GetPlug()->MaxNChannels(ERoute::kInput);
  int nouts = _this->GetPlug()->MaxNChannels(ERoute::kOutput);
  
  double* pInputBufferD = static_cast<double*>(pInputBuffer);
  double* pOutputBufferD = static_cast<double*>(pOutputBuffer);

  bool startWait = _this->mVecWait >= APP_N_VECTOR_WAIT; // wait APP_N_VECTOR_WAIT * iovs before processing audio, to avoid clicks
  bool doFade = _this->mVecWait == APP_N_VECTOR_WAIT || _this->mAudioEnding;
  
  if (startWait && !_this->mAudioDone)
  {
    if (doFade)
      ApplyFades(pInputBufferD, nins, nFrames, _this->mAudioEnding);
    
    for (int i = 0; i < nFrames; i++)
    {
      _this->mBufIndex %= APP_SIGNAL_VECTOR_SIZE;

      if (_this->mBufIndex == 0)
      {
        for (int c = 0; c < nins; c++)
        {
          _this->mInputBufPtrs.Set(c, (pInputBufferD + (c * nFrames)) + i);
        }
        
        for (int c = 0; c < nouts; c++)
        {
          _this->mOutputBufPtrs.Set(c, (pOutputBufferD + (c * nFrames)) + i);
        }
        
        _this->mIPlug->AppProcess(_this->mInputBufPtrs.GetList(), _this->mOutputBufPtrs.GetList(), APP_SIGNAL_VECTOR_SIZE);

        _this->mSamplesElapsed += APP_SIGNAL_VECTOR_SIZE;
      }
      
      for (int c = 0; c < nouts; c++)
      {
        pOutputBufferD[c * nFrames + i] *= APP_MULT;
      }

      _this->mBufIndex++;
    }
    
    if (doFade)
      ApplyFades(pOutputBufferD, nouts, nFrames, _this->mAudioEnding);
    
    if (_this->mAudioEnding)
      _this->mAudioDone = true;
  }
  else
  {
    memset(pOutputBufferD, 0, nFrames * nouts * sizeof(double));
  }

  _this->mVecWait = std::min(_this->mVecWait + 1, uint32_t(APP_N_VECTOR_WAIT + 1));

  return 0;
}

// static
void IPlugAPPHost::MIDICallback(double deltatime, std::vector<uint8_t>* pMsg, void* pUserData)
{
  IPlugAPPHost* _this = (IPlugAPPHost*) pUserData;
  
  if (pMsg->size() == 0 || _this->mExiting)
    return;
  
  if (pMsg->size() > 3)
  {
    if (pMsg->size() > MAX_SYSEX_SIZE)
    {
      DBGMSG("SysEx message exceeds MAX_SYSEX_SIZE\n");
      return;
    }
    
    SysExData data { 0, static_cast<int>(pMsg->size()), pMsg->data() };
    
    _this->mIPlug->mSysExMsgsFromCallback.Push(data);
    return;
  }
  else if (pMsg->size())
  {
    IMidiMsg msg;
    msg.mStatus = pMsg->at(0);
    pMsg->size() > 1 ? msg.mData1 = pMsg->at(1) : msg.mData1 = 0;
    pMsg->size() > 2 ? msg.mData2 = pMsg->at(2) : msg.mData2 = 0;

    _this->mIPlug->mMidiMsgsFromCallback.Push(msg);
  }
}

// static
void IPlugAPPHost::ErrorCallback(RtAudioErrorType type, const std::string &errorText)
{
  std::cerr << "\nerrorCallback: " << errorText << "\n\n";
}

