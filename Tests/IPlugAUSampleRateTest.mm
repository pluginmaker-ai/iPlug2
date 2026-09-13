#include <AudioToolbox/AudioToolbox.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "IPlugAU.h"

using namespace iplug;

namespace {
constexpr double kToneHz = 440.0;
constexpr double kTwoPi = 6.28318530717958647692;
bool gEffect = false;
double gInputPhase = 0.;
double gInputRate = 48000.;

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
  : IPlugAU(InstanceInfo(), Config(1, 1, gEffect ? "2-2" : "0-2", "AU Rate Test", "AU Rate Test",
            "iPlug2 Tests", 0x10000, 'Rate', 'IpTs', 0, !gEffect, false, false,
            true, gEffect ? 0 : 1, false, 0, 0, false, 0, 0, 0, 0, "org.iplug2.rate-test", ""))
  {
    GetParam(0)->InitDouble("Gain", 0.25, 0., 1., 0.001);
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

  void ProcessMidiMsg(const IMidiMsg& msg) override
  {
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
      std::printf("Testing %s\n", effect ? "effect" : "instrument");
      RunLifecycle(unit);
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
