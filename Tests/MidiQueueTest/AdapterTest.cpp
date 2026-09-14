#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "MidiQueueProbe.h"
#include "NativeModule.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace iplug;

static std::atomic<bool> gInProcess {false};
static std::atomic<int> gAllocations {0};
void* operator new(std::size_t size)
{
  if (gInProcess) ++gAllocations;
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static void Check(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

// Real SDK interfaces; all fixture storage is populated before process().
#define FIXTURE_UNKNOWN \
  tresult PLUGIN_API queryInterface(const TUID, void** object) override { *object = nullptr; return kNoInterface; } \
  uint32 PLUGIN_API addRef() override { return 1; } \
  uint32 PLUGIN_API release() override { return 1; }

struct TestPoint { int32 offset; double value; bool readable = true; };
struct Queue final : IParamValueQueue
{
  ParamID id;
  std::vector<TestPoint> points;
  int reads = 0;
  explicit Queue(ParamID param) : id(param) {}
  FIXTURE_UNKNOWN
  ParamID PLUGIN_API getParameterId() override { return id; }
  int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points.size()); }
  tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& value) override
  {
    ++reads;
    if (index < 0 || index >= getPointCount() || !points[index].readable) return kResultFalse;
    offset = points[index].offset;
    value = points[index].value;
    return kResultTrue;
  }
  tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32& index) override
  {
    index = getPointCount();
    points.push_back({offset, value});
    return kResultTrue;
  }
};

struct Changes final : IParameterChanges
{
  std::vector<std::unique_ptr<Queue>> queues;
  FIXTURE_UNKNOWN
  int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(queues.size()); }
  IParamValueQueue* PLUGIN_API getParameterData(int32 index) override { return queues.at(index).get(); }
  IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override
  {
    index = getParameterCount();
    queues.push_back(std::make_unique<Queue>(id));
    return queues.back().get();
  }
  Queue& Add(ParamID id, std::initializer_list<TestPoint> points)
  {
    int32 index;
    auto* queue = static_cast<Queue*>(addParameterData(id, index));
    queue->points = points;
    return *queue;
  }
};

struct Events final : IEventList
{
  std::vector<Event> events;
  int failIndex = -1;
  FIXTURE_UNKNOWN
  int32 PLUGIN_API getEventCount() override { return static_cast<int32>(events.size()); }
  tresult PLUGIN_API getEvent(int32 index, Event& event) override
  {
    if (index == failIndex) return kResultFalse;
    event = events.at(index);
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Event& event) override { events.push_back(event); return kResultOk; }
  void Note(int offset, int channel, bool on, int pitch = 60)
  {
    Event event {};
    event.type = on ? Event::kNoteOnEvent : Event::kNoteOffEvent;
    event.sampleOffset = offset;
    if (on) event.noteOn = {static_cast<int16>(channel), static_cast<int16>(pitch), 0.f, 1.f, 0, -1};
    else event.noteOff = {static_cast<int16>(channel), static_cast<int16>(pitch), 0.f, -1, 0.f};
    events.push_back(event);
  }
};

static ParamID CC(int channel, int controller)
{
  return kMIDICCParamStartIdx + channel * kCountCtrlNumber + controller;
}

static NativeModule* gNativeModule = nullptr;
struct Host
{
  MidiQueueProbe* plug = nullptr;
  IAudioProcessor* processor = nullptr;
  IComponent* component = nullptr;
  std::vector<float> left, right;
  explicit Host(int frames = 8192, int mode = kRealtime) : left(frames), right(frames)
  {
    if (gNativeModule)
      plug = gNativeModule->Create(processor);
    else
    {
      plug = new MidiQueueProbe(InstanceInfo {});
      processor = static_cast<IAudioProcessor*>(plug);
    }
    Check(processor->queryInterface(IComponent::iid, reinterpret_cast<void**>(&component)) == kResultOk, "component unavailable");
    Check(component->initialize(nullptr) == kResultOk, "initialize failed");
    ProcessSetup setup {mode, kSample32, frames, 48000.};
    Check(processor->setupProcessing(setup) == kResultOk, "setup failed");
    Check(component->activateBus(kAudio, Steinberg::Vst::kOutput, 0, true) == kResultOk, "output bus activation failed");
    Check(component->setActive(true) == kResultOk, "activation failed");
    Check(processor->setProcessing(true) == kResultOk, "start failed");
  }
  ~Host()
  {
    processor->setProcessing(false);
    component->setActive(false);
    component->terminate();
    component->release();
    processor->release();
  }
  void Run(Changes* changes = nullptr, Events* events = nullptr, int frames = -1)
  {
    float* buffers[] {left.data(), right.data()};
    AudioBusBuffers output {};
    output.numChannels = 2;
    output.channelBuffers32 = buffers;
    ProcessData data {};
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames < 0 ? static_cast<int32>(left.size()) : frames;
    data.numOutputs = 1;
    data.outputs = &output;
    data.inputParameterChanges = changes;
    data.inputEvents = events;
    gAllocations = 0;
    gInProcess = true;
    const auto result = processor->process(data);
    gInProcess = false;
    Check(result == kResultOk, "process failed");
    Check(gAllocations == 0, "C++ allocation during process");
    Check(!plug->mOverflow, "probe pending buffer overflow");
  }
};

static void Expect(const MidiQueueProbe& plug, int index, int offset, int status, int data1, int data2)
{
  Check(index < plug.mReceivedCount, "missing MIDI message");
  const auto& msg = plug.mReceived[index];
  if (msg.mOffset != offset || msg.mStatus != status || msg.mData1 != data1 || msg.mData2 != data2)
  {
    std::cerr << "message " << index << ": got " << msg.mOffset << "/" << int(msg.mStatus)
              << "/" << int(msg.mData1) << "/" << int(msg.mData2) << '\n';
    throw std::runtime_error("MIDI offset/order/value mismatch");
  }
}

static void TestInternalParameters()
{
  Host host;
  IEditController* controller = nullptr;
  Check(host.component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&controller)) == kResultOk,
        "controller unavailable");
  std::vector<ParamID> ids;
  for (int index = 0; index < controller->getParameterCount(); ++index)
  {
    ParameterInfo info {};
    Check(controller->getParameterInfo(index, info) == kResultOk, "parameter info failed");
    if (info.id < kBypassParam) ids.push_back(info.id);
  }
  Check(ids == std::vector<ParamID>({0, 2}), "internal IDs advertised or surviving IDs renumbered");
  for (const ParamID id : {1u, 3u, 4u})
  {
    Check(controller->setParamNormalized(id, 0.99) != kResultOk, "retired controller write accepted");
    Changes changes;
    changes.Add(id, {{12, 0.99}});
    host.plug->mParamOffset = -1;
    host.Run(&changes);
    Check(host.plug->mParamOffset == -1, "retired automation dispatched");
  }
  Check(host.plug->GetParam(1)->Value() == 0.4 && host.plug->GetParam(2)->Value() == 0.6,
        "retired automation changed state or surviving control");
  Changes active;
  active.Add(2, {{23, 0.75}});
  host.Run(&active);
  Check(host.plug->mParamOffset == 23 && host.plug->GetParam(2)->Value() == 0.75,
        "sparse active automation failed");
  IByteChunk legacy;
  for (double value : {0.2, 0.3, 0.7, 0.9}) legacy.Put(&value);
  Check(host.plug->UnserializeParams(legacy, 0) == 4 * sizeof(double), "legacy state cursor changed");
  // The real state adapter invokes this refresh after decoding every internal slot.
  host.plug->UpdateParams(host.plug, 0);
  Check(std::abs(controller->getParamNormalized(2) - 0.7) < 1.e-6, "state refreshed wrong sparse ID");
  IByteChunk saved;
  Check(host.plug->SerializeParams(saved) && saved.Size() == legacy.Size(), "state slots lost");
  Check(std::memcmp(saved.GetData(), legacy.GetData(), saved.Size()) == 0, "state values moved");
  controller->release();
}

static void TestOrdering()
{
  Host host;
  Changes changes;
  changes.Add(CC(1, 22), {{20, 1.}, {70, 0.}, {70, 1.}});
  changes.Add(CC(0, 64), {{10, 1.}, {70, 0.}, {100, 1.}});
  changes.Add(CC(15, 64), {{20, 0.}, {71, 1.}});
  Events events;
  events.Note(5, 0, true);
  events.Note(20, 1, true);
  events.Note(70, 0, false);
  events.Note(99, 1, false);
  host.Run(&changes, &events);
  Check(host.plug->mReceivedCount == 12, "not every point delivered");
  Expect(*host.plug, 0, 5, 0x90, 60, 127);
  Expect(*host.plug, 1, 10, 0xb0, 64, 127);
  Expect(*host.plug, 2, 20, 0xb1, 22, 127);
  Expect(*host.plug, 3, 20, 0xbf, 64, 0);
  Expect(*host.plug, 4, 20, 0x91, 60, 127);
  Expect(*host.plug, 5, 70, 0xb1, 22, 0);
  Expect(*host.plug, 6, 70, 0xb1, 22, 127);
  Expect(*host.plug, 7, 70, 0xb0, 64, 0);
  Expect(*host.plug, 8, 70, 0x80, 60, 0);
  Expect(*host.plug, 9, 71, 0xbf, 64, 127);
  Expect(*host.plug, 10, 99, 0x81, 60, 0);
  Expect(*host.plug, 11, 100, 0xb0, 64, 127);
  for (const auto& queue : changes.queues) Check(queue->reads == queue->getPointCount(), "point re-read or skipped");
  host.plug->mReceivedCount = 0;
  host.Run();
  Check(host.plug->mReceivedCount == 0, "host pointers retained across blocks");
}

static void TestParametersAndErrors()
{
  Host host;
  Changes changes;
  changes.queues.push_back(nullptr);
  auto& empty = changes.Add(CC(0, 22), {});
  changes.Add(CC(0, 64), {{1, 0., false}, {2, 1.}, {3, 0., false}, {4, 0.}});
  auto& gain = changes.Add(0, {{1, 0.2}, {25, 0.75}});
  changes.Add(kBypassParam, {{0, 0.}, {30, 1.}});
  changes.Add(CC(16, 0), {{1, 1.}}); // unadvertised, invalid channel
  changes.Add(static_cast<ParamID>(-1), {{1, 1.}});
  Events events;
  events.Note(1, 0, true);
  events.Note(3, 0, true);
  events.failIndex = 0;
  host.Run(&changes, &events);
  Check(host.plug->GetBypassed(), "bypass last-point behavior changed");
  Check(host.plug->GetParam(0)->Value() == 0.75 && host.plug->mParamOffset == 25, "ordinary parameter behavior changed");
  Check(empty.reads == 0 && gain.reads == 1, "empty/latest-point parameter traversal changed");
  Check(host.plug->mReceivedCount == 3, "failed getters blocked later events");
  Expect(*host.plug, 0, 2, 0xb0, 64, 127);
  Expect(*host.plug, 1, 3, 0x90, 60, 127);
  Expect(*host.plug, 2, 4, 0xb0, 64, 0);
  Changes unbypass;
  unbypass.Add(kBypassParam, {{0, 0.}});
  host.Run(&unbypass);
  Check(!host.plug->GetBypassed(), "bypass release failed");
}

static void TestOtherMessages()
{
  Host host;
  Changes changes;
  changes.Add(CC(2, kAfterTouch), {{1, 0.}, {5, 1.}});
  changes.Add(CC(3, kPitchBend), {{2, 0.}, {6, 0.5}, {9, 1.}});
  Events events;
  Event pressure {};
  pressure.type = Event::kPolyPressureEvent;
  pressure.sampleOffset = 3;
  pressure.polyPressure = {4, 60, 1.f, -1};
  events.events.push_back(pressure);
  const uint8 bytes[] {0xf0, 0x7d, 0x01, 0xf7};
  Event sysex {};
  sysex.type = Event::kDataEvent;
  sysex.sampleOffset = 5;
  sysex.data = {sizeof(bytes), DataEvent::kMidiSysEx, bytes};
  events.events.push_back(sysex);
  events.Note(6, 3, true);
  events.Note(6, 3, false);
  host.Run(&changes, &events);
  Check(host.plug->mReceivedCount == 8, "pressure/bend/event message lost");
  Expect(*host.plug, 0, 1, 0xd2, 0, 0);
  Expect(*host.plug, 1, 2, 0xe3, 0, 0);
  Expect(*host.plug, 2, 3, 0xa4, 60, 127);
  Expect(*host.plug, 3, 5, 0xd2, 127, 0);
  Expect(*host.plug, 4, 6, 0xe3, 0, 64);
  Expect(*host.plug, 5, 6, 0x93, 60, 127);
  Expect(*host.plug, 6, 6, 0x83, 60, 0);
  Expect(*host.plug, 7, 9, 0xe3, 127, 127);
  Check(host.plug->mSysExOffset == 5 && host.plug->mSysExAfterMidiCount == 4, "SysEx ordering changed");
}

static void TestDenseAndAllChannels()
{
  Host host;
  Changes changes;
  const int controllers = kCountCtrlNumber;
  for (int channel = 15; channel >= 0; --channel)
    for (int cc = controllers - 1; cc >= 0; --cc)
      changes.Add(CC(channel, cc), {{1, 0.}, {2, 1.}});
  host.Run(&changes);
  Check(host.plug->mReceivedCount == 16 * controllers * 2, "full MIDI namespace dropped points");
  for (int i = 0; i < 16 * controllers; ++i)
  {
    Check(host.plug->mReceived[i].mOffset == 1, "all-channel merge failed");
    Check(host.plug->mReceived[i + 16 * controllers].mOffset == 2, "all-channel second point lost");
  }
  Host dense;
  Changes many;
  auto& queue = many.Add(CC(0, 64), {});
  for (int i = 0; i < 20000; ++i) queue.points.push_back({i / 3, static_cast<double>(i % 2)});
  dense.Run(&many);
  Check(dense.plug->mReceivedCount == 20000, "dense queue was truncated");
  Check(queue.reads == 20000, "dense traversal is not linear in queue points");
  for (int i = 0; i < 20000; ++i) Expect(*dense.plug, i, i / 3, 0xb0, 64, (i % 2) * 127);
}

static void TestFlushAndSinglePoint()
{
  Host host;
  Changes changes;
  changes.Add(CC(0, 64), {{0, 1.}});
  host.Run(&changes, nullptr, 0);
  Expect(*host.plug, 0, 0, 0xb0, 64, 127);
  Events events;
  events.Note(0, 0, true);
  events.Note(1, 0, false);
  host.Run(nullptr, &events);
  Check(host.left[100] > 0.01f, "zero-frame controller flush was lost");
}

static void TestMusicalBlocks()
{
  for (const bool ordinaryValues : {false, true})
   for (const int mode : {kRealtime, kOffline})
    for (const int blockSize : {32, 512, 1024, 8192})
    {
      Host host(blockSize, mode);
      // Float normalization reproduces host conversion loss as well as queue loss.
      const double pedalDown = ordinaryValues ? 64.f / 127.f : 1.;
      const double pedalUp = ordinaryValues ? 63.f / 127.f : 0.;
      const double patchBeforeNote = ordinaryValues ? 7.f / 127.f : 1.;
      const double patchAfterNote = ordinaryValues ? 4.f / 127.f : 0.;
      const float latchedLevel = static_cast<float>(0.02 * (1. + (ordinaryValues ? 7. / 127. : 1.)));
      // The audit's pedal sequence: down at 1ms, note 2-4ms, up at 141ms.
      // The second note must latch the high patch even when CC22 is later reset.
      const int total = 16384;
      for (int start = 0; start < total; start += blockSize)
      {
        Changes changes;
        auto& pedal = changes.Add(CC(0, 64), {});
        auto& patch = changes.Add(CC(0, 22), {});
        for (const TestPoint p : {TestPoint{48, pedalDown}, TestPoint{6768, pedalUp}})
          if (p.offset >= start && p.offset < start + blockSize) pedal.points.push_back({p.offset - start, p.value});
        for (const TestPoint p : {TestPoint{480, patchBeforeNote}, TestPoint{5760, patchAfterNote}})
          if (p.offset >= start && p.offset < start + blockSize) patch.points.push_back({p.offset - start, p.value});
        Events events;
        const int offsets[] {96, 192, 960, 7200};
        for (int i = 0; i < 4; ++i)
          if (offsets[i] >= start && offsets[i] < start + blockSize)
            events.Note(offsets[i] - start, 0, i % 2 == 0, i < 2 ? 60 : 64);
        host.Run(&changes, &events);
        for (int i = 0; i < blockSize; ++i)
        {
          const int sample = start + i;
          const float expected = (sample >= 96 && sample < 6768 ? 0.02f : 0.f)
                               + (sample >= 960 && sample < 7200 ? latchedLevel : 0.f);
          Check(std::abs(host.left[i] - expected) < 1.e-6f, "sustain/patch depends on block size or mode");
          Check(host.left[i] == host.right[i], "stereo mismatch");
        }
      }
    }
}

int main(int argc, char** argv)
{
  try
  {
    std::unique_ptr<NativeModule> module;
    if (argc == 2)
    {
      module = std::make_unique<NativeModule>(argv[1]);
      gNativeModule = module.get();
      std::cout << "Testing actual VST3 binary: " << argv[1] << '\n';
    }
    TestInternalParameters(); std::cout << "PASS: sparse VST3 IDs, rejected obsolete automation, compatible legacy state\n";
    TestOrdering(); std::cout << "PASS: all CC points, channels, chronological merge and stable ties\n";
    TestParametersAndErrors(); std::cout << "PASS: ordinary params, bypass, empty queues and failed getters\n";
    TestOtherMessages(); std::cout << "PASS: pitch bend, pressure, SysEx and same-offset note order\n";
    TestDenseAndAllChannels(); std::cout << "PASS: full MIDI namespace and 20,000-point allocation-free block\n";
    TestFlushAndSinglePoint(); std::cout << "PASS: zero-frame flush and single-point controller\n";
    TestMusicalBlocks(); std::cout << "PASS: sustain/patch with endpoints and ordinary CC values at 32/512/1024/8192 frames, realtime/offline\n";
    std::cout << "RESULT: pass\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    gInProcess = false;
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
