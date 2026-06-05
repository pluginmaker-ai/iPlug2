/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

// Strong definition of the Faust per-stage probe seam (Stream G).
//
// Eve's codegen emits a weak stub of `iplug_sentry_probe_on_stage_event`
// in every generated Plugin.cpp. When IPLUG_USE_SENTRY=ON, this TU
// provides the strong override that wins at link time (regardless of
// toolchain — clang/gcc honour __attribute__((weak)) precedence, MSVC
// honours /alternatename). When IPLUG_USE_SENTRY=OFF, this TU is empty
// and the weak stub stays — the plugin links cleanly either way.

#ifdef IPLUG_USE_SENTRY

  #include <cstdint>

  #include "IPlugSentryEventRing.h"

  // Default-visibility marker so a future `-fvisibility=hidden` in the
  // iPlug2_Sentry target can't accidentally hide the override symbol that
  // every generated Plugin.cpp links against.
  #if defined(_WIN32)
    #define IPLUG_PROBE_SEAM_EXPORT
  #else
    #define IPLUG_PROBE_SEAM_EXPORT __attribute__((visibility("default")))
  #endif

namespace
{
  // Map eve's eventType wire enum to our EventKind. Wire contract is
  // verified against Stream G's `iplug2oos-overlay/shared/StageProbe.h`:
  //   enum EventType { kNaN = 0, kSilence = 1, kCpuOver80 = 2 };
  // Anything we don't recognise becomes EVENT_KIND_UNKNOWN so the
  // dashboard still sees *something* and we can grow the taxonomy without
  // breaking older generated plugins.
  iplug::sentry::eventring::EventKind MapKind(int eventType)
  {
    using namespace iplug::sentry::eventring;
    switch (eventType)
    {
      case 0: return EVENT_KIND_PROBE_NAN;
      case 1: return EVENT_KIND_PROBE_SILENCE;
      case 2: return EVENT_KIND_PROBE_CPU_OVER;
      default: return EVENT_KIND_UNKNOWN;
    }
  }
} // anonymous namespace

// Anchor referenced from IPlugSentry.cpp's Init() so the static-archive
// member that contains this strong override is forced into the link. Without
// this, neither macOS ld64 nor MSVC link.exe pulls IPlugSentryProbeSeam.o
// into the plugin DLL when eve's weakly-defined stub already resolves the
// symbol from Plugin.cpp.o — the strong override silently never wins.
extern "C" IPLUG_PROBE_SEAM_EXPORT void iplug_sentry_probe_seam_anchor() {}

extern "C" IPLUG_PROBE_SEAM_EXPORT void iplug_sentry_probe_on_stage_event(
    const char* stageName, int eventType, int channel, float value)
{
  using namespace iplug::sentry::eventring;

  // Drop on the floor when the drain thread hasn't started yet (consent
  // denied, no DSN, PLUGINMAKER_EVENT_RING_DISABLE set, init not finished).
  // Same kill-switch pattern as watchdog::Tick. Without this the ring fills
  // forever during the first ~seconds of plugin life and we account
  // megabytes of legitimate probes as overflow.
  if (!IsDrainRunning()) return;

  // Intern is audio-thread-safe; returns 0 ("unknown") when the table is
  // full or the name is null/empty. We tolerate id=0 silently — the
  // drain side renders it as <unknown> rather than dropping the event.
  const uint32_t stageId = InternStageName(stageName);

  EventRecord rec{};
  // Use the same clock source as the audio detectors so the rendered
  // audio_events.log is monotonically sortable across record sources.
  rec.timestampNs = NowNs();
  rec.kind        = (uint16_t) MapKind(eventType);
  // Channel is int in the seam ABI but int16 in the record. Clamp so
  // a buggy probe emitting -1 stays -1 and a huge channel index doesn't
  // wrap into a misleading value.
  if (channel > 32767) rec.channel = 32767;
  else if (channel < -32768) rec.channel = -32768;
  else rec.channel = (int16_t) channel;
  rec.value       = value;
  rec.stageId     = stageId;
  rec.reserved0   = 0;
  rec.reserved1   = 0;

  // Push is wait-free. A full-ring drop is silently accounted into the
  // overflow counter — the drain thread synthesises a RING_OVERFLOW
  // record on its next wakeup.
  (void) Push(rec);
}

#endif // IPLUG_USE_SENTRY
// OFF build: this TU is NOT compiled (iPlug2_Sentry is INTERFACE in the OFF
// CMakeLists branch). Eve's weak no-op stub in the generated Plugin.cpp
// satisfies every link. No "defense-in-depth" duplicate strong def — keep
// the source honest with the build system.
