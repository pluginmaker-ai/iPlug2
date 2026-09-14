#include "MidiQueueProbe.h"
#include "IPlug_include_in_plug_src.h"

using namespace iplug;

MidiQueueProbe::MidiQueueProbe(const InstanceInfo& info)
: Plugin(info, MakeConfig(4, 1))
{
  GetParam(0)->InitDouble("Gain", 1., 0., 1., 0.01);
  GetParam(1)->InitDouble("Retired middle", 0.4, 0., 1., 0.01, "", IParam::kFlagInternal);
  GetParam(2)->InitDouble("Surviving later ID", 0.6, 0., 1., 0.01);
  GetParam(3)->InitDouble("Retired last", 0.8, 0., 1., 0.01, "", IParam::kFlagInternal);
  MakeDefaultPreset("Default", 1);
}

void MidiQueueProbe::OnReset()
{
  mVoices = {};
  mPedal = {};
  mPatch = {};
  mLevel = 0.;
  mPendingCount = mReceivedCount = 0;
  mSysExOffset = mSysExAfterMidiCount = mParamOffset = -1;
  mOverflow = false;
}

void MidiQueueProbe::OnParamChange(int, EParamSource, int offset)
{
  mParamOffset = offset;
}

void MidiQueueProbe::ProcessMidiMsg(const IMidiMsg& msg)
{
  if (mReceivedCount < kCapacity)
    mReceived[mReceivedCount++] = msg;
  // Capture may fill during a long validator run; only pending overflow loses
  // musical input. The focused tests check the exact captured count separately.
  if (mPendingCount < kCapacity)
    mPending[mPendingCount++] = msg;
  else
    mOverflow = true;
}

void MidiQueueProbe::ProcessSysEx(const ISysEx& msg)
{
  mSysExOffset = msg.mOffset;
  mSysExAfterMidiCount = mReceivedCount;
}

void MidiQueueProbe::Apply(const IMidiMsg& msg)
{
  const int channel = msg.Channel();
  const auto status = msg.StatusMsg();
  auto& voices = mVoices[channel];
  if (status == IMidiMsg::kNoteOn || status == IMidiMsg::kNoteOff)
  {
    auto& voice = voices[msg.NoteNumber()];
    if (status == IMidiMsg::kNoteOn && msg.Velocity() > 0)
    {
      mLevel -= voice.mLevel;
      voice.mHeld = true;
      voice.mLevel = 0.02 * (1. + mPatch[channel] / 127.);
      mLevel += voice.mLevel;
    }
    else
    {
      voice.mHeld = false;
      if (!mPedal[channel])
      {
        mLevel -= voice.mLevel;
        voice.mLevel = 0.;
      }
    }
  }
  else if (status == IMidiMsg::kControlChange)
  {
    if (msg.mData1 == 22)
      mPatch[channel] = msg.mData2;
    else if (msg.mData1 == 64)
    {
      mPedal[channel] = msg.mData2 >= 64;
      if (!mPedal[channel])
      {
        for (auto& voice : voices)
        {
          if (!voice.mHeld)
          {
            mLevel -= voice.mLevel;
            voice.mLevel = 0.;
          }
        }
      }
    }
    else if (msg.mData1 == 120 || msg.mData1 == 123)
    {
      for (auto& voice : voices)
      {
        mLevel -= voice.mLevel;
        voice = {};
      }
    }
  }
}

void MidiQueueProbe::ProcessBlock(sample**, sample** outputs, int nFrames)
{
  int next = 0;
  const double gain = GetParam(0)->Value();
  for (int frame = 0; frame < nFrames; frame++)
  {
    while (next < mPendingCount && mPending[next].mOffset <= frame)
      Apply(mPending[next++]);
    for (int channel = 0; channel < NOutChansConnected(); channel++)
      outputs[channel][frame] = static_cast<sample>(mLevel * gain);
  }
  // Hosts can send MIDI in a zero-frame flush. Keep it for the next audio call.
  int remaining = 0;
  while (next < mPendingCount)
  {
    auto msg = mPending[next++];
    msg.mOffset -= nFrames;
    mPending[remaining++] = msg;
  }
  mPendingCount = remaining;
}

#ifdef VST3_API
// Test-only accessor: resolve RTTI inside the loaded bundle, where type identity
// is defined. The host still calls process() through its SDK interface pointer.
extern "C" EXPORT MidiQueueProbe* GetMidiQueueProbeForTest(Steinberg::Vst::IAudioProcessor* processor)
{
  return dynamic_cast<MidiQueueProbe*>(processor);
}
#endif
