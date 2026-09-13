#include "IPlugVST3_ProcessorBase.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <cstdio>
#include <cstdlib>

using namespace iplug;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

void Require(bool condition, const char* label, int value = -1)
{
  if (!condition)
  {
    std::fprintf(stderr, "FAIL: %s (value=%d)\n", label, value);
    std::exit(EXIT_FAILURE);
  }
}

Config TestConfig()
{
  return Config(0, 0, "0-2", "MIDI test", "MIDI test", "iPlug2", 1, 1, 1, 0,
                true, true, false, false, kInstrument, false, 0, 0, false,
                0, 0, 0, 0, "", "");
}

class TestAPI final : public IPlugAPIBase
{
public:
  TestAPI() : IPlugAPIBase(TestConfig(), kAPIVST3) {}
  void BeginInformHostOfParamChangeFromUI(int) override {}
  void EndInformHostOfParamChangeFromUI(int) override {}
};

class CaptureProcessor final : public IPlugVST3ProcessorBase
{
public:
  explicit CaptureProcessor(TestAPI& api) : IPlugVST3ProcessorBase(TestConfig(), api) {}
  void ProcessMidiMsg(const IMidiMsg& msg) override { mLast = msg; ++mCount; }
  IMidiMsg mLast;
  int mCount = 0;
};

// Stack-owned host fixtures. Production code consumes their actual VST3 interfaces;
// no adapter methods or MIDI conversion code are copied into the test.
template <typename Interface>
class HostInterface : public Interface
{
public:
  tresult PLUGIN_API queryInterface(const TUID, void** object) override
  {
    *object = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
};

class ParameterQueue final : public HostInterface<IParamValueQueue>
{
public:
  ParamID mID = 0;
  ParamValue mValue = 0.;
  int32 mOffset = 0;

  ParamID PLUGIN_API getParameterId() override { return mID; }
  int32 PLUGIN_API getPointCount() override { return 1; }
  tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override
  {
    if (index != 0) return kInvalidArgument;
    offset = mOffset;
    value = mValue;
    return kResultOk;
  }
  tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kNotImplemented; }
};

class ParameterChanges final : public HostInterface<IParameterChanges>
{
public:
  ParameterQueue mQueue;
  int32 PLUGIN_API getParameterCount() override { return 1; }
  IParamValueQueue* PLUGIN_API getParameterData(int32 index) override { return index == 0 ? &mQueue : nullptr; }
  IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override { return nullptr; }
};

class EventList final : public HostInterface<IEventList>
{
public:
  Event mEvent {};
  int32 PLUGIN_API getEventCount() override { return 1; }
  tresult PLUGIN_API getEvent(int32 index, Event& event) override
  {
    if (index != 0) return kInvalidArgument;
    event = mEvent;
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Event&) override { return kNotImplemented; }
};

void CheckDelivery(CaptureProcessor& processor, IPlugQueue<IMidiMsg>& queue,
                   IMidiMsg::EStatusMsg status, int channel, int data1, int data2, int offset)
{
  IMidiMsg forwarded;
  Require(processor.mCount == 1, "one MIDI callback", data2);
  Require(queue.Pop(forwarded), "processor/editor forwarding", data2);
  for (const auto& msg : {processor.mLast, forwarded})
  {
    Require(msg.mStatus == ((static_cast<int>(status) << 4) | channel), "status/channel", data2);
    Require(msg.mData1 == data1 && msg.mData2 == data2, "exact MIDI bytes", data2);
    Require(msg.mOffset == offset, "sample offset", data2);
  }
  Require(!queue.Pop(forwarded), "no duplicate forwarding", data2);
  processor.mCount = 0;
}

void CheckControllers(CaptureProcessor& processor, IPlugQueue<IMidiMsg>& queue)
{
  ParameterChanges changes;
  ProcessData data {};
  data.inputParameterChanges = &changes;
  for (int value = 0; value < 128; ++value)
  {
    const double normalizedValues[] = {
      static_cast<float>(value) / 127.f,
      static_cast<double>(value) / 127.,
      static_cast<float>(value) * (1.f / 127.f),
      static_cast<double>(value) * (1. / 127.),
    };
    for (double normalized : normalizedValues)
    {
      for (int channel = 0; channel < 16; ++channel)
      {
        for (int controller : {22, 64, static_cast<int>(kAfterTouch)})
        {
          changes.mQueue.mID = kMIDICCParamStartIdx + channel * kCountCtrlNumber + controller;
          changes.mQueue.mValue = normalized;
          changes.mQueue.mOffset = 1 + value;
          processor.ProcessParameterChanges(data, queue);
          const bool aftertouch = controller == kAfterTouch;
          CheckDelivery(processor, queue, aftertouch ? IMidiMsg::kChannelAftertouch : IMidiMsg::kControlChange,
                        channel, aftertouch ? value : controller, aftertouch ? 0 : value, 1 + value);
        }
      }
    }
  }
  std::puts("PASS: real VST3 parameter processing: CC22, CC64 and channel aftertouch, all values/channels");
}

void CheckEvents(CaptureProcessor& processor, IPlugQueue<IMidiMsg>& queue)
{
  EventList events;
  IPlugQueue<IMidiMsg> editorQueue(8);
  for (int value = 0; value < 128; ++value)
  {
    for (float normalized : {static_cast<float>(value) / 127.f, static_cast<float>(value) * (1.f / 127.f)})
    {
      for (int16 channel = 0; channel < 16; ++channel)
      {
        events.mEvent = {};
        events.mEvent.type = Event::kNoteOnEvent;
        events.mEvent.sampleOffset = 19;
        events.mEvent.noteOn.pitch = 60;
        events.mEvent.noteOn.velocity = normalized;
        events.mEvent.noteOn.channel = channel;
        processor.ProcessMidiIn(&events, editorQueue, queue);
        CheckDelivery(processor, queue, IMidiMsg::kNoteOn, channel, 60, value, 19);

        events.mEvent = {};
        events.mEvent.type = Event::kPolyPressureEvent;
        events.mEvent.sampleOffset = 23;
        events.mEvent.polyPressure.pitch = 61;
        events.mEvent.polyPressure.pressure = normalized;
        events.mEvent.polyPressure.channel = channel;
        processor.ProcessMidiIn(&events, editorQueue, queue);
        CheckDelivery(processor, queue, IMidiMsg::kPolyAftertouch, channel, 61, value, 23);
      }
    }
  }
  std::puts("PASS: real VST3 event processing: note velocity and poly aftertouch, all values/channels");
}

} // namespace

int main()
{
  TestAPI api;
  CaptureProcessor processor(api);
  IPlugQueue<IMidiMsg> queue(8);
  CheckControllers(processor, queue);
  CheckEvents(processor, queue);
  std::puts("PASS: VST3 MIDI conversion regression (no DAW or audio files)");
}
