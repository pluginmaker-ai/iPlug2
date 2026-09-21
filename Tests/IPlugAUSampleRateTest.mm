#include <AudioToolbox/AudioToolbox.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

#include "IPlugAU.h"

using namespace iplug;

namespace {
constexpr double kToneHz = 440.0;
constexpr double kTwoPi = 6.28318530717958647692;
bool gEffect = false;
double gInputPhase = 0.;
double gInputRate = 48000.;
bool gAdmissionTest = false;

void Require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

void Check(OSStatus status, const char* operation)
{
  if (status != noErr)
  {
    std::fprintf(stderr, "%s: %d\n", operation, static_cast<int>(status));
    throw std::runtime_error(operation);
  }
}

class RateTestPlugin final : public IPlugAU
{
public:
  RateTestPlugin()
  : IPlugAU(InstanceInfo(), Config(4, 1, gEffect ? "2-2" : "0-2", "AU Rate Test", "AU Rate Test",
            "iPlug2 Tests", 0x10000, 'Rate', 'IpTs', 0, !gEffect, false, false,
            true, gEffect ? 0 : 1, false, 0, 0, false, 0, 0, 0, 0, "org.iplug2.rate-test", ""))
  {
    GetParam(0)->InitDouble("Gain", 0.25, 0., 1., 0.001);
    GetParam(1)->InitDouble("Retired middle", 0.4, 0., 1., 0.001, "", IParam::kFlagInternal);
    GetParam(2)->InitDouble("Surviving later ID", 0.6, 0., 1., 0.001);
    GetParam(3)->InitDouble("Retired last", 0.8, 0., 1., 0.001, "", IParam::kFlagInternal);
    MakeDefaultPreset();
  }

  void OnReset() override
  {
    mDSPRate = GetSampleRate();
    mDSPBlockSize = GetBlockSize();
    ++mResets;
  }

  void OnActivate(bool active) override
  {
    if (active) mRateAtActivation = mDSPRate;
  }

  void OnParamChange(int paramIdx, EParamSource source, int sampleOffset) override
  {
    if (source == kHost) mHostChanges.push_back({paramIdx, sampleOffset, GetParam(paramIdx)->Value()});
  }

  bool UsesRenderAdmission() const override { return gAdmissionTest; }
  ERenderAdmission PrepareRender(int, bool offline) override
  {
    mLastOffline = offline;
    ++mAdmissions;
    if (offline && mWaitForReady) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (mRequestedAdmission == ERenderAdmission::Silence && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return mRequestedAdmission;
  }
  void OnRenderEventOverflow() override { mRequestedAdmission = ERenderAdmission::Error; }
  std::atomic<ERenderAdmission> mRequestedAdmission{ERenderAdmission::Ready};
  bool mWaitForReady = false;
  bool mLastOffline = false;
  int mAdmissions = 0;
  std::vector<IMidiMsg> mReceived;

  void ProcessMidiMsg(const IMidiMsg& msg) override
  {
    if (gAdmissionTest) mReceived.push_back(msg);
    if (msg.StatusMsg() == IMidiMsg::kNoteOn) mPlaying = msg.Velocity() > 0;
    if (msg.StatusMsg() == IMidiMsg::kNoteOff) mPlaying = false;
  }

  void ProcessBlock(sample** inputs, sample** outputs, int frames) override
  {
    for (int i = 0; i < frames; ++i)
    {
      if (gEffect)
      {
        outputs[0][i] = inputs[0][i] * GetParam(0)->Value();
        outputs[1][i] = inputs[1][i] * GetParam(0)->Value();
        continue;
      }
      const double value = mPlaying ? GetParam(0)->Value() * std::sin(mPhase) : 0.;
      outputs[0][i] = value;
      outputs[1][i] = value * 0.5;
      mPhase = std::fmod(mPhase + kTwoPi * kToneHz / mDSPRate, kTwoPi);
    }
  }

  double mDSPRate = 44100.;
  double mRateAtActivation = 0.;
  int mDSPBlockSize = 0;
  int mResets = 0;
  bool mPlaying = false;
  double mPhase = 0.;
  struct HostChange { int id; int offset; double value; };
  std::vector<HostChange> mHostChanges;
};

RateTestPlugin* gPlugin = nullptr;

OSStatus InputTone(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                   UInt32, UInt32 frames, AudioBufferList* buffers)
{
  for (UInt32 i = 0; i < frames; ++i)
  {
    const float value = std::sin(gInputPhase);
    static_cast<float*>(buffers->mBuffers[0].mData)[i] = value;
    static_cast<float*>(buffers->mBuffers[1].mData)[i] = value * 0.5f;
    gInputPhase = std::fmod(gInputPhase + kTwoPi * kToneHz / gInputRate, kTwoPi);
  }
  return noErr;
}

AudioStreamBasicDescription Format(double rate, UInt32 channels = 2)
{
  AudioStreamBasicDescription format{};
  format.mSampleRate = rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
  format.mBytesPerPacket = sizeof(float);
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = sizeof(float);
  format.mChannelsPerFrame = channels;
  format.mBitsPerChannel = 32;
  return format;
}

OSStatus SetFormat(AudioUnit unit, const AudioStreamBasicDescription& format,
                   AudioUnitElement bus = 0)
{
  return AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, bus, &format, sizeof(format));
}

void VerifyInternalParameters(AudioUnit unit)
{
  UInt32 size = 0;
  Check(AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList,
      kAudioUnitScope_Global, 0, &size, nullptr), "parameter list size");
  Require(size == 2 * sizeof(AudioUnitParameterID), "internal parameters advertised");
  AudioUnitParameterID ids[2]{};
  Check(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList,
      kAudioUnitScope_Global, 0, ids, &size), "sparse parameter list");
  Require(ids[0] == 0 && ids[1] == 2, "surviving IDs changed");
  for (AudioUnitParameterID id : {1u, 3u, 4u, 0xffffffffu})
  {
    AudioUnitParameterValue value = -1;
    Require(AudioUnitGetParameter(unit, id, kAudioUnitScope_Global, 0, &value)
        == kAudioUnitErr_InvalidParameter, "retired or invalid read accepted");
    Require(AudioUnitSetParameter(unit, id, kAudioUnitScope_Global, 0, 0.99, 0)
        == kAudioUnitErr_InvalidParameter, "retired automation accepted");
    if (id < 4)
    {
      AudioUnitParameterInfo info{};
      size = sizeof(info);
      Require(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo,
          kAudioUnitScope_Global, id, &info, &size) == kAudioUnitErr_InvalidParameter,
          "retired metadata remains public");
    }
  }
  Require(gPlugin->GetParam(2)->Value() == 0.6, "obsolete automation reached a surviving ID");
  Check(AudioUnitSetParameter(unit, 2, kAudioUnitScope_Global, 0, 0.75, 0), "sparse active write");
  IByteChunk legacy;
  for (double value : {0.2, 0.3, 0.7, 0.9}) legacy.Put(&value);
  Require(gPlugin->UnserializeParams(legacy, 0) == 4 * sizeof(double), "legacy state cursor changed");
  IByteChunk saved;
  Require(gPlugin->SerializeParams(saved) && saved.Size() == legacy.Size(), "internal state slots lost");
  Require(std::memcmp(saved.GetData(), legacy.GetData(), legacy.Size()) == 0, "legacy state values moved");
  AudioUnitParameterValue restored = 0;
  Check(AudioUnitGetParameter(unit, 2, kAudioUnitScope_Global, 0, &restored), "restored sparse read");
  Require(std::abs(restored - 0.7) < 1.e-6, "surviving value restored from wrong slot");

  auto immediate = [](AudioUnitParameterID id, float value, UInt32 offset) {
    AudioUnitParameterEvent event{};
    event.scope = kAudioUnitScope_Global;
    event.parameter = id;
    event.eventType = kParameterEvent_Immediate;
    event.eventValues.immediate.value = value;
    event.eventValues.immediate.bufferOffset = offset;
    return event;
  };
  auto verifyBatch = [&](std::vector<AudioUnitParameterEvent> events,
                         std::vector<RateTestPlugin::HostChange> expected) {
    gPlugin->mHostChanges.clear();
    Check(AudioUnitScheduleParameters(unit, events.data(), events.size()), "mixed scheduled automation");
    Require(gPlugin->mHostChanges.size() == expected.size(), "active scheduled events lost or internal events delivered");
    for (size_t i = 0; i < expected.size(); ++i)
    {
      const auto& got = gPlugin->mHostChanges[i];
      Require(got.id == expected[i].id && got.offset == expected[i].offset
          && std::abs(got.value - expected[i].value) < 1.e-6, "scheduled event value, order or offset changed");
      Require(std::abs(gPlugin->GetParam(got.id)->Value() - expected[i].value) < 1.e-6,
          "scheduled active value not applied");
    }
    Require(gPlugin->GetParam(1)->Value() == 0.3 && gPlugin->GetParam(3)->Value() == 0.9,
        "scheduled automation changed internal state");
  };
  verifyBatch({immediate(1, 0.99f, 7), immediate(2, 0.45f, 19)}, {{2, 19, 0.45}});
  verifyBatch({immediate(0, 0.35f, 3), immediate(3, 0.99f, 11), immediate(2, 0.65f, 27)},
      {{0, 3, 0.35}, {2, 27, 0.65}});
  for (AudioUnitParameterID id : {4u, 0xffffffffu})
  {
    auto invalid = immediate(id, 0.99f, 0);
    Require(AudioUnitScheduleParameters(unit, &invalid, 1) == kAudioUnitErr_InvalidParameter,
        "invalid scheduled ID accepted");
  }
  auto wrongScope = immediate(1, 0.99f, 0);
  wrongScope.scope = kAudioUnitScope_Input;
  Require(AudioUnitScheduleParameters(unit, &wrongScope, 1) == kAudioUnitErr_InvalidProperty,
      "invalid scheduled scope accepted");
  std::puts("PASS: mixed retired/active AU automation preserves active values and offsets");
  std::puts("PASS: sparse AU IDs, rejected obsolete automation, compatible legacy state");
}

void VerifyTone(AudioUnit unit, double rate, double& sampleTime)
{
  Require(gPlugin->mDSPRate == rate, "DSP configuration is stale");
  gInputRate = rate;
  if (!gEffect) Check(MusicDeviceMIDIEvent(unit, 0x90, 69, 100, 0), "note on");
  constexpr UInt32 frames = 256;
  std::vector<float> left(frames), right(frames);
  struct { UInt32 count; AudioBuffer buffers[2]; } buffers{
    2, {{1, frames * sizeof(float), left.data()}, {1, frames * sizeof(float), right.data()}}};
  std::vector<double> crossings;
  float previous = 0.;
  double peak = 0.;
  for (int block = 0; block < 32; ++block)
  {
    AudioTimeStamp time{};
    time.mFlags = kAudioTimeStampSampleTimeValid;
    time.mSampleTime = sampleTime;
    AudioUnitRenderActionFlags flags = 0;
    Check(AudioUnitRender(unit, &flags, &time, 0, frames,
                          reinterpret_cast<AudioBufferList*>(&buffers)), "render");
    for (UInt32 i = 0; i < frames; ++i)
    {
      Require(std::isfinite(left[i]) && std::isfinite(right[i]), "non-finite sample");
      Require(std::abs(right[i] - left[i] * 0.5f) < 1.e-6f, "stereo channels changed");
      peak = std::max(peak, std::abs(static_cast<double>(left[i])));
      if (previous < 0.f && left[i] >= 0.f)
        crossings.push_back(block * frames + i - left[i] / (left[i] - previous));
      previous = left[i];
    }
    sampleTime += frames;
  }
  Require(crossings.size() > 10, "tone is silent");
  const double pitch = rate * (crossings.size() - 1) / (crossings.back() - crossings.front());
  std::printf("rate=%.0f pitch=%.5f peak=%.5f\n", rate, pitch, peak);
  Require(std::abs(pitch - kToneHz) < 0.05, "DSP uses stale sample rate");
  Require(std::abs(peak - 0.37) < 0.001, "host gain did not survive lifecycle");
  if (!gEffect) Check(MusicDeviceMIDIEvent(unit, 0x80, 69, 0, 0), "note off");
}

void VerifyAdmission(AudioUnit unit)
{
  using Admission = IPlugProcessor::ERenderAdmission;
  gAdmissionTest = true;
  Check(AudioUnitInitialize(unit), "admission initialize");
  constexpr UInt32 frames = 128;
  float left[frames]{}, right[frames]{};
  struct { UInt32 count; AudioBuffer buffers[2]; } buffers{
    2, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
  AudioTimeStamp time{};
  time.mFlags = kAudioTimeStampSampleTimeValid;
  auto render = [&] {
    AudioUnitRenderActionFlags flags = 0;
    const auto result = AudioUnitRender(unit, &flags, &time, 0, frames,
        reinterpret_cast<AudioBufferList*>(&buffers));
    time.mSampleTime += frames;
    return result;
  };
  gPlugin->mRequestedAdmission = Admission::Silence;
  Check(MusicDeviceMIDIEvent(unit, 0x90, 60, 90, 37), "stage live note");
  Require(gPlugin->mReceived.empty(), "MIDI reached engine before admission");
  Check(render(), "silent realtime block");
  Require(gPlugin->mReceived.empty(), "unready realtime MIDI delivered");
  for (float value : left) Require(value == 0, "unready block not silent");
  gPlugin->mRequestedAdmission = Admission::Ready;
  Check(render(), "ready block after dropped live note");
  Require(gPlugin->mReceived.empty(), "elapsed realtime note replayed");

  UInt32 offline = 1;
  Check(AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender,
      kAudioUnitScope_Global, 0, &offline, sizeof(offline)), "offline property");
  gPlugin->mRequestedAdmission = Admission::Silence;
  gPlugin->mWaitForReady = true;
  Check(MusicDeviceMIDIEvent(unit, 0x90, 60, 90, 37), "first queued note");
  Check(MusicDeviceMIDIEvent(unit, 0xb0, 64, 127, 0), "earlier queued sustain");
  Check(MusicDeviceMIDIEvent(unit, 0x80, 60, 0, 37), "same-offset note off");
  std::thread loader([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gPlugin->mRequestedAdmission = Admission::Ready;
  });
  const auto result = render();
  loader.join();
  Check(result, "delayed offline render");
  Require(gPlugin->mLastOffline, "offline flag not observed at admission");
  Require(gPlugin->mReceived.size() == 3, "offline events lost");
  Require(gPlugin->mReceived[0].mOffset == 0 && gPlugin->mReceived[0].mStatus == 0xb0,
      "event offsets not sorted");
  Require(gPlugin->mReceived[1].mOffset == 37 && gPlugin->mReceived[1].mStatus == 0x90 &&
      gPlugin->mReceived[2].mOffset == 37 && gPlugin->mReceived[2].mStatus == 0x80,
      "original offsets or stable ties changed");
  gPlugin->mReceived.clear();
  Check(MusicDeviceMIDIEvent(unit, 0x90, 60, 90, 2), "note before reset");
  Check(AudioUnitReset(unit, kAudioUnitScope_Global, 0), "reset pending events");
  Check(render(), "render after reset");
  Require(gPlugin->mReceived.empty(), "reset retained prior events");
  for (int i = 0; i < 2048; ++i)
    Check(MusicDeviceMIDIEvent(unit, 0x90, 60, 90, 0), "fill bounded queue");
  Require(MusicDeviceMIDIEvent(unit, 0x90, 60, 90, 0) != noErr, "overflow was silent");
  Require(render() == kAudioUnitErr_CannotDoInCurrentContext, "render error swallowed");
  Require(gPlugin->mReceived.empty(), "overflow partially delivered notes");
  gPlugin->mRequestedAdmission = Admission::Ready;
  offline = 0;
  Check(AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender,
      kAudioUnitScope_Global, 0, &offline, sizeof(offline)), "back to realtime");
  Check(render(), "live after offline");
  Require(!gPlugin->mLastOffline && gPlugin->mReceived.empty(), "mode transition replayed old events");
  Check(AudioUnitUninitialize(unit), "admission uninitialize");
  gAdmissionTest = false;
  std::puts("PASS: AU admission precedes MIDI, preserves offsets, drops elapsed live blocks, clears reset, reports overflow");
}

void RunLifecycle(AudioUnit unit)
{
  Check(AudioUnitSetParameter(unit, 0, kAudioUnitScope_Global, 0, 0.37, 0), "set gain");
  if (gEffect)
  {
    auto format = Format(48000.);
    Check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input, 0, &format, sizeof(format)), "initial input format");
    AURenderCallbackStruct callback{InputTone, nullptr};
    Check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &callback, sizeof(callback)), "input callback");
  }
  Check(SetFormat(unit, Format(48000.)), "initial format");
  const int beforeInit = gPlugin->mResets;
  Check(AudioUnitInitialize(unit), "initial initialize");
  Require(gPlugin->mResets > beforeInit, "initialize did not reset DSP");
  Require(gPlugin->mRateAtActivation == 48000., "activation preceded DSP configuration");
  double sampleTime = 0.;
  VerifyTone(unit, 48000., sampleTime);

  for (double rate : {44100., 96000., 48000., 44100., 48000.})
  {
    Check(AudioUnitUninitialize(unit), "uninitialize");
    Check(SetFormat(unit, Format(rate)), "change stream format");
    Check(AudioUnitInitialize(unit), "reinitialize");
    VerifyTone(unit, rate, sampleTime);
  }

  Check(SetFormat(unit, Format(96000.)), "active stream format change");
  VerifyTone(unit, 96000., sampleTime);
  const int beforeRejected = gPlugin->mResets;
  Check(SetFormat(unit, Format(96000.)), "unchanged stream format");
  Check(SetFormat(unit, Format(0.)), "unspecified sample rate");
  Require(gPlugin->mResets == beforeRejected, "unchanged rate reset DSP");
  auto invalid = Format(22050.);
  invalid.mFormatID = kAudioFormatMPEG4AAC;
  Require(SetFormat(unit, invalid) != noErr, "invalid format accepted");
  Require(SetFormat(unit, Format(22050., 3)) != noErr, "invalid channels accepted");
  Require(SetFormat(unit, Format(22050.), 5) != noErr, "invalid bus accepted");
  Require(gPlugin->mResets == beforeRejected, "rejected format reset DSP");
  VerifyTone(unit, 96000., sampleTime);

  UInt32 blockSize = 1024;
  Check(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
      kAudioUnitScope_Global, 0, &blockSize, sizeof(blockSize)), "set block size");
  Require(gPlugin->mDSPBlockSize == 1024, "block configuration not applied");
  const int beforeReset = gPlugin->mResets;
  Check(AudioUnitReset(unit, kAudioUnitScope_Global, 0), "explicit reset");
  Require(gPlugin->mResets > beforeReset, "explicit reset stopped working");
  VerifyTone(unit, 96000., sampleTime);
  Check(AudioUnitUninitialize(unit), "final uninitialize");
}
} // namespace

namespace iplug {
IPlugAU* MakePlug(void* memory)
{
  gPlugin = new (memory) RateTestPlugin();
  return gPlugin;
}
} // namespace iplug

extern "C" AudioComponentPlugInInterface* IPlugAURateTestFactory(const AudioComponentDescription* description)
{
  return IPlugAUFactory<RateTestPlugin, true>::Factory(description);
}

#ifndef IPLUG_AU_RATE_TEST_BUNDLE
int main()
{
  AudioComponentDescription description{kAudioUnitType_MusicDevice, 'Rate', 'IpTs', 0, 0};
  AudioUnit unit = nullptr;
  try
  {
    for (bool effect : {false, true})
    {
      gEffect = effect;
      description.componentType = effect ? kAudioUnitType_Effect : kAudioUnitType_MusicDevice;
      const auto factory = effect ? IPlugAUFactory<RateTestPlugin, false>::Factory
                                  : IPlugAUFactory<RateTestPlugin, true>::Factory;
      const auto component = AudioComponentRegister(&description, CFSTR("iPlug2 Tests: AU Rate Test"),
          0x10000, reinterpret_cast<AudioComponentFactoryFunction>(factory));
      Require(component != nullptr, "register component");
      Check(AudioComponentInstanceNew(component, &unit), "create instance");
      VerifyInternalParameters(unit);
      std::printf("Testing %s\n", effect ? "effect" : "instrument");
      RunLifecycle(unit);
      if (!effect) VerifyAdmission(unit);
      Check(AudioComponentInstanceDispose(unit), "dispose instance");
      unit = nullptr;
    }
    std::puts("RESULT: pass — real AU lifecycle, pitch, parameters, stereo and rejected formats");
    return 0;
  }
  catch (const std::exception& error)
  {
    if (unit) AudioComponentInstanceDispose(unit);
    std::fprintf(stderr, "RESULT: fail — %s\n", error.what());
    return 1;
  }
}
#endif
