/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#pragma once

#include <cstdint>

#include "IPlugSentryWatchdog.h"  // SlotHandle is reused here

/**
 * @file
 * @brief Audio-thread edge detectors for transient anomalies
 *        (Surface A — A-6).
 *
 * The watchdog detects when ProcessBlock has STOPPED ticking. The audio
 * detectors detect when ProcessBlock is STILL ticking but producing
 * anomalous output — the kind of failure that doesn't wedge the audio
 * thread but still ruins the user's day:
 *
 *   - Block CPU over 80% of the budgeted wall-clock window. Edge-latched
 *     so a sustained overload emits one event per overload episode, not
 *     one per block.
 *   - NaN or Inf in the output buffers. The plugin processed but the
 *     samples are toxic — every host downstream will silence or denormal-
 *     flush them, often after a few seconds of glitching.
 *
 * Detections push records into the per-DLL SPSC event ring (see
 * IPlugSentryEventRing.h). The drain thread renders those records into
 * a circular text buffer that the watchdog + crash hooks attach to
 * outgoing Sentry events as `audio_events.log`.
 *
 * Why these two, why only these two
 * ---------------------------------
 *   - xrun detection: there is no portable mechanism. Many hosts deliver
 *     xruns via callbacks the plugin doesn't subscribe to; on some, the
 *     xrun never reaches the plugin at all. Skipped for v1.
 *   - Denormal flush counters: requires per-platform FPU/SSE flush-to-
 *     zero register reads. Cost > value for v1.
 *   - DC offset, clip count: caller-visible via existing meters; not a
 *     reliability signal worth a Sentry event.
 *
 * CPU timing source
 * -----------------
 *   - macOS: `mach_absolute_time` — converted via `mach_timebase_info`
 *     to ns. The conversion factor is cached at first use; the read
 *     itself is a vDSO-equivalent call (no syscall).
 *   - Windows: `QueryPerformanceCounter` — vDSO on Windows 10+. We
 *     accept the theoretical risk that a future Windows revision moves
 *     QPC back into a syscall; if so, every plugin host in the industry
 *     has the same problem and we'll cross that bridge then.
 *   - The frequency / mach_timebase numerator+denominator are read once,
 *     cached in process-static storage, and never touched again from
 *     the audio thread.
 *
 * Load-percent formula
 *
 *     elapsed_ns = ProcessBlockEnd_time - ProcessBlockBegin_time
 *     budget_ns  = (blockSize * 1e9) / sampleRate
 *     load_pct   = elapsed_ns / budget_ns
 *
 *   `load_pct > 0.8` is the over-threshold condition. Latched per slot:
 *   we only emit on the rising edge of "over" and clear the latch only
 *   after N consecutive blocks come in clean (N = 8 — a small hysteresis
 *   so a borderline plugin doesn't flap on every block).
 *
 * NaN/Inf scan
 * ------------
 *   Per-block work is capped. We sample the first, middle, and last
 *   frame of each output channel — three reads per channel, ≤ 32
 *   channels, ≤ 96 reads. Only when one of those samples is non-finite
 *   do we run the full scan to attribute the event to a specific
 *   channel + frame index. The full scan path costs ~nFrames * nChannels
 *   reads but is gated on a real hit, so the steady-state cost is
 *   bounded.
 *
 *   We use `std::isnan` / `std::isinf` (header-only, compile to a few
 *   bit-ops on every supported toolchain — verified to NOT touch any
 *   global state). The OFF build never includes this header so OFF-
 *   build callers don't pull `<cmath>` transitively from us either.
 *
 * Threading contract
 * ------------------
 *   - `ProcessBlockBegin` and `ProcessBlockEnd` run on the AUDIO thread,
 *     once per `IPlugProcessor::ProcessBuffers` call. They MUST be
 *     real-time-safe: no allocations, no locks, no syscalls, no kernel
 *     calls. The only persistent state mutated is the per-slot latch
 *     fields (atomics, relaxed ordering — they're only ever read +
 *     written by the audio thread for the same slot).
 *   - The two functions form a Begin/End pair scoped around the call
 *     to ProcessBlock. They cooperate via the per-slot timing fields
 *     stored in the watchdog HeartbeatSlot — we reuse the watchdog's
 *     per-instance slot rather than allocating a parallel table.
 *
 * Compile-time gating
 * -------------------
 *   When IPLUG_USE_SENTRY is NOT defined, ProcessBlockBegin and both
 *   ProcessBlockEnd overloads are inline empty functions. IPlugProcessor
 *   guards its own call sites with `#ifdef IPLUG_USE_SENTRY` too — the
 *   inline empty pattern is belt-and-braces for any caller that drops
 *   the guard.
 */

namespace iplug
{
namespace sentry
{
namespace audiodetectors
{

  // SlotHandle is shared with the watchdog — same per-IPlugProcessor
  // identity is used for hang detection AND for CPU/NaN edge detection.
  // Reusing the handle keeps the data path lean: one slot per instance,
  // one register/unregister pair at instance lifecycle, no duplicate
  // bookkeeping. The detectors store their own latch state inside a
  // small sub-struct attached to the HeartbeatSlot (see .cpp).
  using SlotHandle = ::iplug::sentry::watchdog::SlotHandle;

#ifdef IPLUG_USE_SENTRY

  /** Audio-thread: record the start-of-block timestamp for this slot.
   *
   *  Called immediately BEFORE the host-side ProcessBlock invocation in
   *  `IPlugProcessor::ProcessBuffers`. Real-time-safe:
   *    - one mach_absolute_time / QPC read
   *    - one relaxed store of the cached ns timestamp into the slot
   *
   *  A zero-valued handle is silently ignored (fast-path bail). A handle
   *  whose generation no longer matches the slot's is also ignored — the
   *  same use-after-unregister defense the watchdog Tick uses.
   */
  void ProcessBlockBegin(SlotHandle handle) noexcept;

  /** Audio-thread: close out the block, run edge detectors, push events.
   *
   *  Called immediately AFTER ProcessBlock returns. Computes elapsed_ns
   *  against the timestamp stored by ProcessBlockBegin, derives load_pct
   *  from blockSize + sampleRate, and runs the BLOCK_CPU_OVER_80 edge
   *  detector. Then sample-scans the output buffers (first/middle/last
   *  frame of each channel); if any non-finite values are found, runs a
   *  full scan to locate the first NaN/Inf and pushes one
   *  NAN_OUTPUT / INF_OUTPUT event with the attributed channel + frame.
   *
   *  Two overloads cover the IPlug `sample` type's float-vs-double
   *  dispatch — IPlug compiles with either PLUG_SAMPLE_DST = float or
   *  double depending on target. Keeping the API non-templated keeps
   *  the header lightweight and avoids dragging `<cmath>` /
   *  IPlugProcessor's heavy includes into every TU that calls in.
   *
   *  Both overloads are real-time-safe: edge detection is two relaxed
   *  loads + a relaxed compare + a relaxed store. NaN/Inf fast-path is
   *  3 reads per channel. Event push is the SPSC ring (32B memcpy +
   *  one release store), wait-free.
   *
   *  Zero/stale handles are silently ignored; null buffers (nChannels
   *  == 0 or pOutputs == nullptr) skip the NaN/Inf scan but still run
   *  the CPU edge detector.
   *
   *  @param handle     Slot handle (matches the one used by the watchdog)
   *  @param pOutputs   Output channel pointers (may be nullptr)
   *  @param nChannels  Number of output channels (≤ 32 expected; larger
   *                    is supported but only the first 32 are scanned)
   *  @param nFrames    Frames in this block (used to bound the scan)
   *  @param blockSize  Block size hint reported by the host (used for
   *                    the load_pct budget — may differ from nFrames
   *                    on hosts that pass partial buffers)
   *  @param sampleRate Sample rate hint reported by the host (Hz)
   */
  void ProcessBlockEnd(SlotHandle handle,
                       const float* const* pOutputs,
                       int32_t nChannels,
                       int32_t nFrames,
                       int32_t blockSize,
                       double sampleRate) noexcept;

  void ProcessBlockEnd(SlotHandle handle,
                       const double* const* pOutputs,
                       int32_t nChannels,
                       int32_t nFrames,
                       int32_t blockSize,
                       double sampleRate) noexcept;

#else // !IPLUG_USE_SENTRY — every entry point is a no-op.

  inline void ProcessBlockBegin(SlotHandle /*handle*/) noexcept {}

  inline void ProcessBlockEnd(SlotHandle /*handle*/,
                              const float* const* /*pOutputs*/,
                              int32_t /*nChannels*/,
                              int32_t /*nFrames*/,
                              int32_t /*blockSize*/,
                              double /*sampleRate*/) noexcept {}

  inline void ProcessBlockEnd(SlotHandle /*handle*/,
                              const double* const* /*pOutputs*/,
                              int32_t /*nChannels*/,
                              int32_t /*nFrames*/,
                              int32_t /*blockSize*/,
                              double /*sampleRate*/) noexcept {}

#endif // IPLUG_USE_SENTRY

} // namespace audiodetectors
} // namespace sentry
} // namespace iplug
