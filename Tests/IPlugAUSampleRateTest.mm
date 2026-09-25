#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <iterator>
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
// A generated Song Keys-style name, 63 bytes, with a two-byte "ü" across the
// 51-byte limit of AudioUnitParameterInfo::name.
constexpr const char* kLongName = "grand_piano_release_trigger_volume_upper_register_überblendung";

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
    GetParam(2)->InitDouble(kLongName, 0.6, 0., 1., 0.001, "dB");
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

void NoteListener(void*, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement) {}

struct HostInputCase
{
  const char* name;
  std::function<void()> run;
};

// Each case runs on its own so one refusal cannot hide another. The name is
// printed before the case runs, so a crash still names it. Set
// IPLUG_AU_ONLY_CASE=<name> to run a single case.
int gCasesRun = 0;

void RunCases(const char* suite, std::initializer_list<HostInputCase> cases)
{
  const char* only = std::getenv("IPLUG_AU_ONLY_CASE");
  int failed = 0;
  for (const auto& hostCase : cases)
  {
    if (only && std::strcmp(only, hostCase.name) != 0) continue;
    ++gCasesRun;
    std::printf("  case %s\n", hostCase.name);
    std::fflush(stdout);
    try { hostCase.run(); }
    catch (const std::exception& error)
    {
      ++failed;
      std::printf("  FAIL %s: %s\n", hostCase.name, error.what());
    }
  }
  std::fflush(stdout);
  Require(failed == 0, suite);
}

// Names, labels and groups longer than their old 32-byte fields are stored
// whole (up to 127 bytes), never run into the next field, and are cut only at
// a UTF-8 character boundary.
void VerifyParamNameStorage()
{
  auto stored = [](const std::string& name, const char* label, const char* group) {
    auto param = std::make_unique<IParam>();
    param->InitDouble(name.c_str(), 0., 0., 1., 0.01, label, 0, group);
    return std::make_tuple(std::string(param->GetName()), std::string(param->GetLabel()),
        std::string(param->GetGroup()));
  };
  const std::string songKeys = "grand_piano_release_trigger_volume";
  RunCases("IParam name storage", {
    {"name-over-31-bytes", [&] {
      Require(stored(songKeys, "dB", "Grand Piano") == std::make_tuple(songKeys, std::string("dB"),
          std::string("Grand Piano")), "34-byte name ran into its label");
    }},
    {"name-over-63-bytes", [&] {
      const std::string name = songKeys + "_" + std::string(35, 'x');
      Require(stored(name, "frames", "Juno") == std::make_tuple(name, std::string("frames"), std::string("Juno")),
          "70-byte name overran its label");
    }},
    {"name-over-95-bytes", [&] {
      const std::string name = songKeys + "_" + std::string(65, 'y');
      Require(stored(name, "%", "Upright") == std::make_tuple(name, std::string("%"), std::string("Upright")),
          "100-byte name overran its group");
    }},
    {"name-over-127-bytes", [&] {
      const std::string name(200, 'z');
      Require(stored(name, "dB", "Rhodes") == std::make_tuple(name.substr(0, 127), std::string("dB"),
          std::string("Rhodes")), "200-byte name not cut to 127 bytes");
    }},
    {"name-cut-at-utf8-boundary", [&] {
      const std::string name = std::string(126, 'a') + "\xC3\xBC" + "tail"; // "ü" across byte 127
      Require(std::get<0>(stored(name, "", "")) == std::string(126, 'a'), "UTF-8 character cut in half");
    }},
  });
  std::puts("PASS: IParam stores long names, labels and groups whole, bounded and UTF-8 safe");
}

// The host sees a long generated name in full; the fixed 52-byte ParameterInfo
// name is cut at a character boundary.
void VerifyLongParameterName(AudioUnit unit)
{
  RunCases("AU long parameter name", {
    {"long-name-in-parameter-info", [&] {
      AudioUnitParameterInfo info{};
      UInt32 size = sizeof(info);
      Check(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, 2, &info, &size),
          "long parameter info");
      CFStringRef expected = CFStringCreateWithCString(nullptr, kLongName, kCFStringEncodingUTF8);
      const bool whole = info.cfNameString && CFStringCompare(info.cfNameString, expected, 0) == kCFCompareEqualTo;
      CFRelease(expected);
      if (info.cfNameString) CFRelease(info.cfNameString);
      Require(whole, "host did not get the whole long name");
      Require(std::string(info.name) == std::string(kLongName, 50), "52-byte name field not cut at a character boundary");
    }},
    {"long-name-by-id", [&] {
      AudioUnitParameterIDName idName{2, kAudioUnitParameterName_Full, nullptr};
      UInt32 size = sizeof(idName);
      Check(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterIDName, kAudioUnitScope_Global, 0, &idName, &size),
          "long name by ID");
      CFStringRef expected = CFStringCreateWithCString(nullptr, kLongName, kCFStringEncodingUTF8);
      const bool whole = idName.outName && CFStringCompare(idName.outName, expected, 0) == kCFCompareEqualTo;
      CFRelease(expected);
      if (idName.outName) CFRelease(idName.outName);
      Require(whole, "long name by ID not whole");
    }},
  });
  std::puts("PASS: AU hosts see long parameter names whole, with a UTF-8-safe 52-byte field");
}

// Host-supplied pointers, sizes and indices outside what each property expects:
// every call must be refused or clamped, never crash or write past the host's data.
void VerifyHostPropertyInputs(AudioUnit unit)
{
  RunCases("AU host property input", {
    {"short-get-buffer", [&] {
      struct { AudioUnitParameterID first; UInt32 guard; } shortList{0, 0xfeedfacf};
      UInt32 size = sizeof(shortList.first);
      Check(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
          &shortList, &size), "short parameter list");
      Require(shortList.guard == 0xfeedfacf, "GetProperty wrote past the host's buffer");
      Require(size == sizeof(shortList.first) && shortList.first == 0, "short parameter list contents");
    }},
    {"negative-name-length", [&] {
      AudioUnitParameterIDName idName{0, -5, nullptr};
      UInt32 size = sizeof(idName);
      Check(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterIDName, kAudioUnitScope_Global, 0,
          &idName, &size), "negative desired name length");
      const bool empty = idName.outName && CFStringGetLength(idName.outName) == 0;
      if (idName.outName) CFRelease(idName.outName);
      Require(empty, "negative name length not clamped to empty");
    }},
    {"unknown-clump", [&] {
      AudioUnitParameterNameInfo clump{1, kAudioUnitParameterName_Full, nullptr};
      UInt32 size = sizeof(clump);
      Require(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterClumpName, kAudioUnitScope_Global, 0,
          &clump, &size) == kAudioUnitErr_PropertyNotInUse, "unknown clump ID accepted");
    }},
    {"string-from-current-value", [&] {
      AudioUnitParameterStringFromValue fromValue{0, nullptr, nullptr};
      UInt32 size = sizeof(fromValue);
      Check(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0,
          &fromValue, &size), "string for the current value");
      Require(fromValue.outString != nullptr, "no string for the current value");
      CFRelease(fromValue.outString);
    }},
    {"value-from-null-string", [&] {
      AudioUnitParameterValueFromString fromString{0, nullptr, 0};
      UInt32 size = sizeof(fromString);
      Require(AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterValueFromString, kAudioUnitScope_Global, 0,
          &fromString, &size) == kAudioUnitErr_InvalidPropertyValue, "value from a NULL string accepted");
    }},
    {"null-set-value", [&] {
      Require(AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Global, 0, nullptr, 0) != noErr,
          "NULL property value accepted");
    }},
    {"short-set-value", [&] {
      Float64 rate = 0, unchanged = 0;
      UInt32 size = sizeof(rate);
      Check(AudioUnitGetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Global, 0, &rate, &size), "read rate");
      const UInt32 shortRate = 22050;
      Require(AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Global, 0, &shortRate,
          sizeof(shortRate)) == kAudioUnitErr_InvalidPropertyValue, "short property value accepted");
      size = sizeof(unchanged);
      Check(AudioUnitGetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Global, 0, &unchanged, &size), "reread rate");
      Require(unchanged == rate, "short property value changed the sample rate");
    }},
    {"null-context-name", [&] {
      CFStringRef before = CFSTR("Before"), noName = nullptr;
      Check(AudioUnitSetProperty(unit, kAudioUnitProperty_ContextName, kAudioUnitScope_Global, 0, &before,
          sizeof(before)), "context name");
      Check(AudioUnitSetProperty(unit, kAudioUnitProperty_ContextName, kAudioUnitScope_Global, 0, &noName,
          sizeof(noName)), "NULL context name");
      WDL_String stored;
      gPlugin->GetTrackName(stored);
      Require(stored.GetLength() == 0, "NULL context name did not clear the name");
    }},
    {"non-ascii-context-name", [&] {
      CFStringRef trackName = CFSTR("Flügel – Spur 1");
      Check(AudioUnitSetProperty(unit, kAudioUnitProperty_ContextName, kAudioUnitScope_Global, 0, &trackName,
          sizeof(trackName)), "non-ASCII context name");
      WDL_String stored;
      gPlugin->GetTrackName(stored);
      Require(std::strcmp(stored.Get(), "Flügel – Spur 1") == 0, "non-ASCII context name lost");
    }},
    {"nameless-host-identifier", [&] {
      AUHostIdentifier hostID{};
      Check(AudioUnitSetProperty(unit, kAudioUnitProperty_AUHostIdentifier, kAudioUnitScope_Global, 0, &hostID,
          sizeof(hostID)), "host identifier without a name");
    }},
    {"null-parameter-value", [&] {
      AudioUnitParameterValue value = 0;
      Require(AudioUnitGetParameter(unit, 0, kAudioUnitScope_Global, 0, nullptr) != noErr, "NULL parameter value accepted");
      Check(AudioUnitGetParameter(unit, 0, kAudioUnitScope_Global, 0, &value), "parameter read after NULL");
    }},
    {"null-scheduled-events", [&] {
      Require(AudioUnitScheduleParameters(unit, nullptr, 1) != noErr, "NULL scheduled events accepted");
    }},
    {"null-render-notify", [&] {
      Require(AudioUnitAddRenderNotify(unit, nullptr, nullptr) != noErr, "NULL render notification accepted");
    }},
    {"null-property-listener", [&] {
      Require(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_Latency, nullptr, nullptr) != noErr,
          "NULL property listener accepted");
      Check(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_Latency, NoteListener, nullptr), "real listener");
    }},
    {"sysex-without-data", [&] {
      if (!gEffect) Require(MusicDeviceSysEx(unit, nullptr, 4) != noErr, "SysEx without data accepted");
    }},
  });
  std::puts("PASS: AU properties, parameters and callbacks refuse NULL, short and out-of-range host input");
}

// Render calls naming a missing bus, or more buffers than the plug-in has
// output channels, are refused or cleared instead of reaching unowned memory.
void VerifyRenderBounds(AudioUnit unit, double& sampleTime)
{
  constexpr UInt32 frames = 256;
  std::vector<float> left(frames), right(frames), extra(frames, 1.f);
  struct { UInt32 count; AudioBuffer buffers[3]; } three{
    3, {{1, frames * sizeof(float), left.data()}, {1, frames * sizeof(float), right.data()},
        {1, frames * sizeof(float), extra.data()}}};
  auto render = [&](UInt32 bus) {
    AudioTimeStamp time{};
    time.mFlags = kAudioTimeStampSampleTimeValid;
    time.mSampleTime = sampleTime;
    sampleTime += frames;
    AudioUnitRenderActionFlags flags = 0;
    return AudioUnitRender(unit, &flags, &time, bus, frames, reinterpret_cast<AudioBufferList*>(&three));
  };
  RunCases("AU render bounds", {
    {"unknown-output-bus", [&] {
      Require(render(7) == kAudioUnitErr_InvalidElement, "unknown output bus accepted");
    }},
    {"extra-host-buffer", [&] {
      std::fill(extra.begin(), extra.end(), 1.f);
      three.buffers[2].mData = extra.data();
      Check(render(0), "more host buffers than output channels");
      for (float sample : extra) Require(sample == 0.f, "extra host buffer not cleared");
    }},
    {"extra-buffer-without-data", [&] {
      three.buffers[2].mData = nullptr;
      Require(render(0) == kAudioUnitErr_InvalidPropertyValue, "extra buffer without data pointed at scratch");
    }},
  });
  std::puts("PASS: AU render refuses unknown buses and extra buffers without data, clears extra host buffers");
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

// AudioUnitRender's ioActionFlags is optional (Apple's AUPlugInDispatch
// substitutes local flags); Logic on Intel passes NULL. Buffers may also
// arrive without mData, asking the unit to supply its own.
void VerifyNullHostPointers(AudioUnit unit, AudioTimeStamp& time)
{
  using Admission = IPlugProcessor::ERenderAdmission;
  constexpr UInt32 frames = 128;
  float left[frames], right[frames];
  struct { UInt32 count; AudioBuffer buffers[2]; } buffers{
    2, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
  auto renderWith = [&](AudioUnitRenderActionFlags* flags, AudioBufferList* list) {
    const auto result = AudioUnitRender(unit, flags, &time, 0, frames, list);
    time.mSampleTime += frames;
    return result;
  };
  auto* list = reinterpret_cast<AudioBufferList*>(&buffers);

  gPlugin->mRequestedAdmission = Admission::Silence;
  std::fill(std::begin(left), std::end(left), 1.f);
  Check(renderWith(nullptr, list), "unready block without host flags");
  for (float value : left) Require(value == 0, "unready block without host flags not silent");
  AudioUnitRenderActionFlags flags = 0;
  Check(renderWith(&flags, list), "unready block with host flags");
  Require(flags & kAudioUnitRenderAction_OutputIsSilence, "unready block not flagged silent");

  struct { UInt32 count; AudioBuffer buffers[2]; } unowned{
    2, {{1, sizeof(left), nullptr}, {1, sizeof(right), nullptr}}};
  Check(renderWith(nullptr, reinterpret_cast<AudioBufferList*>(&unowned)), "unready block without host buffers");
  Require(unowned.buffers[0].mData && unowned.buffers[1].mData, "unready block supplied no buffers");
  for (UInt32 i = 0; i < frames; ++i)
    Require(static_cast<float*>(unowned.buffers[0].mData)[i] == 0, "unready supplied buffer not silent");

  gPlugin->mRequestedAdmission = Admission::Ready;
  Check(renderWith(nullptr, list), "ready block without host flags");
  unowned.buffers[0].mData = unowned.buffers[1].mData = nullptr;
  Check(renderWith(nullptr, reinterpret_cast<AudioBufferList*>(&unowned)), "ready block without host buffers");
  Require(unowned.buffers[0].mData && unowned.buffers[1].mData, "ready block supplied no buffers");

  Require(AudioUnitRender(unit, &flags, nullptr, 0, frames, list) != noErr, "missing timestamp accepted");
  Require(AudioUnitRender(unit, &flags, &time, 0, frames, nullptr) != noErr, "missing buffer list accepted");
  std::puts("PASS: AU render accepts NULL host flags and buffers, rejects missing timestamp/list");
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
  VerifyNullHostPointers(unit, time);

  // Offsets outside the block are clamped into it; the block and its note-off
  // survive, and admission never flips to Error.
  gPlugin->mReceived.clear();
  Check(MusicDeviceMIDIEvent(unit, 0x90, 60, 90, 37), "in-block note");
  Check(MusicDeviceMIDIEvent(unit, 0x80, 60, 0, 5000), "note-off past the block");
  Check(MusicDeviceMIDIEvent(unit, 0xb0, 64, 0, 0xffffffffu), "offset past INT_MAX");
  Check(render(), "block with out-of-range offsets");
  Require(gPlugin->mRequestedAdmission == Admission::Ready, "out-of-range offset failed admission");
  Require(gPlugin->mReceived.size() == 3, "out-of-range events dropped");
  // Both late events land on the last frame, after the in-block note, in arrival order.
  const int last = static_cast<int>(frames) - 1;
  Require(gPlugin->mReceived[0].mOffset == 37 && gPlugin->mReceived[0].mStatus == 0x90 &&
      gPlugin->mReceived[1].mOffset == last && gPlugin->mReceived[1].mStatus == 0x80 &&
      gPlugin->mReceived[2].mOffset == last && gPlugin->mReceived[2].mStatus == 0xb0,
      "out-of-range offsets not clamped to the block's last frame");
  gPlugin->mReceived.clear();
  Check(render(), "block after clamped offsets");
  std::puts("PASS: AU clamps out-of-range MIDI offsets into the block without failing it");

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
  Require(AudioUnitRender(unit, nullptr, &time, 0, frames, reinterpret_cast<AudioBufferList*>(&buffers))
      == kAudioUnitErr_CannotDoInCurrentContext, "render error without host flags swallowed");
  time.mSampleTime += frames;
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
  VerifyRenderBounds(unit, sampleTime);

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
    VerifyParamNameStorage();
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
      VerifyHostPropertyInputs(unit);
      VerifyLongParameterName(unit);
      std::printf("Testing %s\n", effect ? "effect" : "instrument");
      RunLifecycle(unit);
      if (!effect) VerifyAdmission(unit);
      Check(AudioComponentInstanceDispose(unit), "dispose instance");
      unit = nullptr;
    }
    const char* only = std::getenv("IPLUG_AU_ONLY_CASE");
    Require(!only || gCasesRun > 0, "IPLUG_AU_ONLY_CASE names no case");
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
