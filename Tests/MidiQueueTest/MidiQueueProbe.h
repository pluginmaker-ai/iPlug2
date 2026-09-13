#pragma once
#include <array>
#include "IPlug_include_in_plug_hdr.h"

// Synthetic instrument: CC64 holds released notes; CC22 selects the amplitude
// latched by the next note. All audio stays in memory, with no external assets.
class MidiQueueProbe final : public iplug::Plugin
{
public:
  explicit MidiQueueProbe(const iplug::InstanceInfo& info);
  void OnReset() override;
  void ProcessMidiMsg(const iplug::IMidiMsg& msg) override;
  void ProcessSysEx(const iplug::ISysEx& msg) override;
  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
  void OnParamChange(int idx, iplug::EParamSource source, int offset) override;

  static constexpr int kCapacity = 65536;
  std::array<iplug::IMidiMsg, kCapacity> mReceived;
  int mReceivedCount = 0;
  int mSysExOffset = -1;
  int mSysExAfterMidiCount = -1;
  int mParamOffset = -1;
  bool mOverflow = false;

private:
  void Apply(const iplug::IMidiMsg& msg);
  struct Voice { bool mHeld = false; double mLevel = 0.; };
  std::array<std::array<Voice, 128>, 16> mVoices {};
  std::array<bool, 16> mPedal {};
  std::array<int, 16> mPatch {};
  std::array<iplug::IMidiMsg, kCapacity> mPending;
  int mPendingCount = 0;
  double mLevel = 0.;
};
