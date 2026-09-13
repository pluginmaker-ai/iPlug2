#include "IPlugMidi.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using iplug::IMidiMsg;

namespace {

void Require(bool condition, const char* label, int value = -1)
{
  // These checks must still execute in Release builds, where assert is disabled.
  if (!condition)
  {
    std::fprintf(stderr, "FAIL: %s (value=%d)\n", label, value);
    std::exit(EXIT_FAILURE);
  }
}

void CheckMessage(const IMidiMsg& msg, IMidiMsg::EStatusMsg status, int data1,
                  int data2, int channel, int offset)
{
  Require(msg.StatusMsg() == status, "message status", data2);
  Require(msg.mStatus == ((static_cast<int>(status) << 4) | channel), "status byte", data2);
  Require(msg.Channel() == channel, "message channel", data2);
  Require(msg.mData1 == data1, "first data byte", data1);
  Require(msg.mData2 == data2, "second data byte", data2);
  Require(msg.mOffset == offset, "sample offset", data2);
}

template <typename T>
void CheckRoundTrips(const char* label, bool multiplyReciprocal)
{
  for (int value = 0; value < 128; ++value)
  {
    // Division matches host inputs; multiplication matches iPlug's VST3 output.
    const T normalized = multiplyReciprocal
      ? static_cast<T>(value) * (T(1) / T(127))
      : static_cast<T>(value) / T(127);
    Require(IMidiMsg::NormalizedTo7Bit(normalized) == value, label, value);

    for (int controller = 0; controller < 128; ++controller)
    {
      const int channel = (controller + value) % 16;
      const int offsets[] = {0, 1, 511};
      const int offset = offsets[(controller + value) % 3];
      const auto cc = static_cast<IMidiMsg::EControlChangeMsg>(controller);
      IMidiMsg msg(-123, 0xFF, 0xFF, 0xFF);
      msg.MakeControlChangeMsg(cc, normalized, channel, offset);
      CheckMessage(msg, IMidiMsg::kControlChange, controller, value, channel, offset);
      Require(msg.ControlChangeIdx() == cc, "controller index", controller);
      Require(msg.ControlChange(cc) == static_cast<double>(value) / 127., "CC getter", value);
      if (cc == IMidiMsg::kSustainOnOff)
        Require(IMidiMsg::ControlChangeOnOff(msg.ControlChange(cc)) == (value >= 64), "sustain threshold", value);
    }
  }
  std::printf("PASS: %s: all 128 values/controllers, varying all 16 channels and 3 offsets\n", label);
}

void CheckBoundaries()
{
  struct Case { double value; int expected; };
  const double infinity = std::numeric_limits<double>::infinity();
  const Case cases[] = {
    {-infinity, 0}, {-std::numeric_limits<double>::max(), 0}, {-1.0, 0},
    {-std::numeric_limits<double>::denorm_min(), 0}, {-0.0, 0}, {0.0, 0},
    {std::numeric_limits<double>::denorm_min(), 0},
    {std::nextafter(0.5 / 127., 0.0), 0}, {0.5 / 127., 1},
    {std::nextafter(0.5, 0.0), 63}, {0.5, 64}, {std::nextafter(0.5, 1.0), 64},
    {std::nextafter(1.0, 0.0), 127}, {1.0, 127}, {std::nextafter(1.0, infinity), 127},
    {2.0, 127}, {std::numeric_limits<double>::max(), 127}, {infinity, 127},
    {std::numeric_limits<double>::quiet_NaN(), 0},
    {-std::numeric_limits<double>::quiet_NaN(), 0},
  };
  for (const auto& item : cases)
  {
    Require(IMidiMsg::NormalizedTo7Bit(item.value) == item.expected, "clamp/non-finite/half-step", item.expected);
    IMidiMsg msg;
    msg.MakeControlChangeMsg(IMidiMsg::kUndefined022, item.value, 15, 123);
    CheckMessage(msg, IMidiMsg::kControlChange, 22, item.expected, 15, 123);
  }
  for (int value = 0; value < 127; ++value)
  {
    Require(IMidiMsg::NormalizedTo7Bit((value + 0.25) / 127.) == value, "round down", value);
    Require(IMidiMsg::NormalizedTo7Bit((value + 0.75) / 127.) == value + 1, "round up", value);
  }
  IMidiMsg msg(-1, 0xFF, 0xFF, 0xFF);
  msg.MakeControlChangeMsg(IMidiMsg::kModWheel, 1.0);
  CheckMessage(msg, IMidiMsg::kControlChange, 1, 127, 0, 0);
  std::puts("PASS: boundaries, nearest rounding, default metadata, infinities and NaN");
}

void CheckIntegerConstructors()
{
  for (int channel = 0; channel < 16; ++channel)
  {
    for (int value = 0; value < 128; ++value)
    {
      IMidiMsg msg;
      msg.MakeNoteOnMsg(60, value, 17, channel);
      CheckMessage(msg, IMidiMsg::kNoteOn, 60, value, channel, 17);
      msg.MakeNoteOffMsg(60, 19, channel);
      CheckMessage(msg, IMidiMsg::kNoteOff, 60, 0, channel, 19);
      msg.MakeChannelATMsg(value, 23, channel);
      CheckMessage(msg, IMidiMsg::kChannelAftertouch, value, 0, channel, 23);
      msg.MakePolyATMsg(61, value, 29, channel);
      CheckMessage(msg, IMidiMsg::kPolyAftertouch, 61, value, channel, 29);
      msg.MakeProgramChange(value, channel, 31);
      CheckMessage(msg, IMidiMsg::kProgramChange, value, 0, channel, 31);
    }
  }
  std::puts("PASS: existing integer note/aftertouch/program constructors on all 16 channels");
}

void CheckPitchWheel()
{
  for (int value = 0; value < 16384; ++value)
  {
    const double bipolar = static_cast<double>(value - 8192) / 8192.;
    const float normalized = static_cast<float>(value) / 16384.f;
    IMidiMsg msg;
    msg.MakePitchWheelMsg(bipolar, 15, 47);
    CheckMessage(msg, IMidiMsg::kPitchWheel, value & 0x7F, value >> 7, 15, 47);
    Require(msg.PitchWheel() == bipolar, "pitch-wheel getter", value);
    msg.MakePitchWheelMsg((static_cast<double>(normalized) * 2.) - 1., 7, 53);
    CheckMessage(msg, IMidiMsg::kPitchWheel, value & 0x7F, value >> 7, 7, 53);
  }
  std::puts("PASS: unchanged pitch-wheel conversion, all 16384 values");
}

} // namespace

int main()
{
  CheckRoundTrips<float>("float division", false);
  CheckRoundTrips<double>("double division", false);
  CheckRoundTrips<float>("float reciprocal multiplication", true);
  CheckRoundTrips<double>("double reciprocal multiplication", true);
  CheckBoundaries();
  CheckIntegerConstructors();
  CheckPitchWheel();
  std::puts("PASS: MIDI conversion regression");
}
