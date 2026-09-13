#pragma once
#include <filesystem>
#include <stdexcept>
#include "pluginterfaces/base/ipluginbase.h"
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#else
#include <windows.h>
#endif

// Loads this test's own VST3 bundle through the SDK factory. The host then uses
// the fixture's capture fields to inspect the actual binary's MIDI callbacks.
class NativeModule
{
public:
  explicit NativeModule(const char* path)
  {
#ifdef __APPLE__
    const auto absolute = std::filesystem::absolute(path).string();
    const auto url = CFURLCreateFromFileSystemRepresentation(nullptr,
      reinterpret_cast<const UInt8*>(absolute.c_str()), absolute.size(), true);
    mBundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!mBundle || !CFBundleLoadExecutable(mBundle))
      throw std::runtime_error("could not load test VST3 bundle");
    const auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(
      CFBundleGetFunctionPointerForName(mBundle, CFSTR("bundleEntry")));
    mExit = reinterpret_cast<bool (*)()>(CFBundleGetFunctionPointerForName(mBundle, CFSTR("bundleExit")));
    const auto getFactory = reinterpret_cast<Steinberg::IPluginFactory* (*)()>(
      CFBundleGetFunctionPointerForName(mBundle, CFSTR("GetPluginFactory")));
    if (!entry || !mExit || !getFactory || !entry(mBundle))
      throw std::runtime_error("missing VST3 entry points");
#else
    mLibrary = LoadLibraryW(std::filesystem::u8path(path).c_str());
    if (!mLibrary) throw std::runtime_error("could not load test VST3 binary");
    const auto entry = reinterpret_cast<bool (*)()>(GetProcAddress(mLibrary, "InitDll"));
    mExit = reinterpret_cast<bool (*)()>(GetProcAddress(mLibrary, "ExitDll"));
    const auto getFactory = reinterpret_cast<Steinberg::IPluginFactory* (*)()>(GetProcAddress(mLibrary, "GetPluginFactory"));
    if (!entry || !mExit || !getFactory || !entry())
      throw std::runtime_error("missing VST3 entry points");
#endif
#ifdef __APPLE__
    mProbe = reinterpret_cast<ProbeAccessor>(CFBundleGetFunctionPointerForName(mBundle, CFSTR("GetMidiQueueProbeForTest")));
#else
    mProbe = reinterpret_cast<ProbeAccessor>(GetProcAddress(mLibrary, "GetMidiQueueProbeForTest"));
#endif
    if (!mProbe) throw std::runtime_error("test fixture accessor unavailable");
    mFactory = getFactory();
    if (!mFactory) throw std::runtime_error("VST3 factory unavailable");
  }

  ~NativeModule()
  {
    if (mFactory) mFactory->release();
    if (mExit) mExit();
#ifdef __APPLE__
    if (mBundle) { CFBundleUnloadExecutable(mBundle); CFRelease(mBundle); }
#else
    if (mLibrary) FreeLibrary(mLibrary);
#endif
  }

  MidiQueueProbe* Create(Steinberg::Vst::IAudioProcessor*& processor)
  {
    Steinberg::PClassInfo info {};
    if (mFactory->getClassInfo(0, &info) != Steinberg::kResultOk)
      throw std::runtime_error("test plugin class unavailable");
    processor = nullptr;
    if (mFactory->createInstance(info.cid, Steinberg::Vst::IAudioProcessor::iid,
                                reinterpret_cast<void**>(&processor)) != Steinberg::kResultOk || !processor)
      throw std::runtime_error("VST3 factory instantiation failed");
    auto* probe = mProbe(processor);
    if (!probe)
    {
      processor->release();
      throw std::runtime_error("expected this build's MidiQueueProbe fixture");
    }
    return probe;
  }

private:
  using ProbeAccessor = MidiQueueProbe* (*)(Steinberg::Vst::IAudioProcessor*);
  ProbeAccessor mProbe = nullptr;
  Steinberg::IPluginFactory* mFactory = nullptr;
  bool (*mExit)() = nullptr;
#ifdef __APPLE__
  CFBundleRef mBundle = nullptr;
#else
  HMODULE mLibrary = nullptr;
#endif
};
