/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#pragma once

#include <cstdint>

/**
 * @file
 * @brief Audio-thread hang watchdog for iPlug2 plugins (Surface A — A-5).
 *
 * Detects when a plugin's audio thread (ProcessBlock) has stopped ticking
 * and emits a Sentry event describing the wedged thread. Per-DLL singleton:
 * one std::thread per loaded plugin DLL, managing a fixed-size table of
 * HeartbeatSlot entries (one slot per IPlugProcessor instance — 128 slots
 * is enough for a 32-voice rack plus headroom).
 *
 * Lifecycle
 * ---------
 *   - The watchdog thread is started lazily by `iplug::sentry::Init()` AFTER
 *     `sentry_init` has succeeded, and only when consent is granted + DSN is
 *     configured. Start() registers a process-lifetime atexit() hook that
 *     calls Stop() with a bounded join timeout so DLL unload never blocks
 *     on a wedged transport.
 *   - There is NO static destructor join. Shutdown is best-effort under a
 *     hard deadline; if the watchdog thread doesn't drain in 200ms it is
 *     detached. This avoids loader-lock deadlocks on Windows DllMain and
 *     atexit ordering races against sentry-native's own transport.
 *
 * Threading contract
 * ------------------
 *   - `Register` / `Unregister` run on the HOST thread during IPlugProcessor
 *     ctor/dtor. They take the slot-allocator mutex (slow path; not RT).
 *   - `Tick` runs on the AUDIO thread, once per `ProcessBuffers` call. It
 *     MUST be real-time-safe:
 *       1 relaxed load of slot generation + compare
 *       1 relaxed fetch_add on tick
 *       3 relaxed stores (blockSize, sampleRate, renderingOffline—release
 *         on renderingOffline so the watchdog acquires a coherent view)
 *     No allocations, no locks, no syscalls, no heap-dependent branches.
 *     The slot lookup is O(1) by handle index.
 *   - `Start` / `Stop` run from `iplug::sentry::Init()` and the registered
 *     atexit hook respectively — both on a non-realtime thread.
 *
 * Runtime kill switch
 * -------------------
 *   Setting environment variable PLUGINMAKER_WATCHDOG_DISABLE to a non-empty,
 *   non-"0" value at process start disables Start() entirely. Useful for
 *   support to mitigate any in-the-wild false-positive storms without
 *   shipping a new build. Read once at Start() time — not per-tick.
 *
 * Compile-time gating
 * -------------------
 *   When IPLUG_USE_SENTRY is NOT defined, every entry point compiles down
 *   to an `inline` empty function returning a zero-initialised SlotHandle.
 *   That guarantees:
 *     - IPlugProcessor.cpp's call sites (gated by their own #ifdef) link
 *       cleanly with no extra symbol surface.
 *     - The OFF build never compiles or links the watchdog TU.
 *     - ProcessBuffers has ZERO added overhead at -O1+.
 *
 * Stall semantics
 * ---------------
 *   A slot is wedged when the watchdog observes, with acquire ordering on
 *   the inUse/renderingOffline gates:
 *       (now - lastChangeAt) > max(200ms, 50 * blockSize / sampleRate)
 *   AND the slot has ticked at least once AND mRenderingOffline is false
 *   AND no offline render was seen within the current episode. Each
 *   stall episode emits at most one Sentry event (de-duped via a
 *   stallEpisodeActive latch that only clears after N consecutive
 *   advancing-tick observations).
 */

namespace iplug
{
namespace sentry
{
namespace watchdog
{

  struct HeartbeatSlot;

  /** Opaque handle to a registered HeartbeatSlot.
   *
   *  Packed {idx, gen}: `idx` is 1-based into the per-DLL slot table
   *  (0 == unregistered sentinel); `gen` is a generation counter bumped
   *  on every Unregister + Register cycle. The audio thread's Tick()
   *  uses `idx` to reach the slot in O(1) and cross-checks `gen` against
   *  the slot's current generation to detect use-after-unregister —
   *  this matters when the host tears down one IPlugProcessor while
   *  another is being constructed on a different thread and the freed
   *  slot index has already been re-issued.
   *
   *  Trivially copyable, 32 bits — IPlugProcessor bit-casts it into a
   *  `void* mWatchdogSlot` reserved member. A zero-valued handle
   *  (`{0, 0}`) means "unregistered" — Register guarantees it never
   *  hands one out (idx is always >= 1, gen is always >= 1), and Tick
   *  treats it as a fast-path no-op.
   */
  struct SlotHandle
  {
    uint16_t idx;
    uint16_t gen;
  };

#ifdef IPLUG_USE_SENTRY

  /** Claim a free slot in the per-DLL heartbeat table.
   *
   *  Called from `IPlugProcessor::IPlugProcessor` on the host thread.
   *  Takes the slot-allocator mutex (slow path; never called on audio).
   *
   *  @returns A SlotHandle valid for subsequent Tick/Unregister calls,
   *           or a zero-valued handle ({0,0}) when the table is full.
   *           The audio thread treats a zero handle as a no-op, so a
   *           full table degrades gracefully — the plugin still runs,
   *           just without hang detection on the overflow instance.
   */
  SlotHandle Register();

  /** Release a previously-claimed slot.
   *
   *  Called from `~IPlugProcessor` on the host thread. Safe to call with
   *  a zero-valued handle (no-op). Bumps the slot's generation counter
   *  so any in-flight Tick from the old owner becomes a no-op via the
   *  handle-vs-slot generation compare in Tick().
   */
  void Unregister(SlotHandle handle);

  /** Audio-thread tick. MUST be real-time-safe.
   *
   *  Called from `IPlugProcessor::ProcessBuffers(PLUG_SAMPLE_DST, nFrames)`
   *  exactly once per block. The steady-state cost is one relaxed load +
   *  compare (generation cross-check), one relaxed fetch_add, two relaxed
   *  stores, and one release-ordered store — all wait-free, lock-free,
   *  no heap, no kernel. The function returns void — failures cannot be
   *  reported back to a hot loop without breaking real-time safety.
   *
   *  A zero-valued handle is silently ignored (fast-path bail). A handle
   *  whose generation no longer matches the slot's (use-after-unregister)
   *  is also silently ignored — protects neighbour slots from cross-
   *  instance tick smearing during plugin teardown/reload races.
   *
   *  @param handle             Handle returned by Register().
   *  @param blockSize          Frames in this block (used by stall threshold).
   *  @param sampleRate         Current sample rate in Hz.
   *  @param renderingOffline   True when host reports offline rendering;
   *                            watchdog will skip stall checks for this slot.
   */
  void Tick(SlotHandle handle, int32_t blockSize, double sampleRate, bool renderingOffline);

  /** Start the per-DLL watchdog thread. Idempotent.
   *
   *  Called from `iplug::sentry::Init()` immediately after `sentry_init`
   *  succeeds, AND only when consent is granted + DSN is non-empty.
   *  Internally guarded by std::call_once; safe from any thread. Reads the
   *  `PLUGINMAKER_WATCHDOG_DISABLE` env var and bails if set — runtime
   *  kill switch for support to mitigate false-positive storms.
   *
   *  Registers an atexit() shutdown hook that calls Stop() with a bounded
   *  join timeout — we do NOT use a static destructor (would race against
   *  sentry-native's own shutdown and risk Windows loader-lock deadlock).
   */
  void Start();

  /** Request the watchdog thread to exit and join (bounded). Idempotent.
   *
   *  Called from the atexit() hook registered in Start(), or explicitly by
   *  tests. Signals gStopRequested, wakes the worker via condition_variable,
   *  joins with a 200ms ceiling. If the join misses the ceiling, the thread
   *  is detached — process exit MUST NOT block on a wedged transport.
   */
  void Stop();

#ifdef IPLUG_SENTRY_WATCHDOG_DEBUG
  /** Debug-only: force a slot to look 10s stale so the next watchdog
   *  iteration fires a stall event. Used by QA to validate the end-to-end
   *  Sentry path without wedging a real audio thread. Linker-stripped from
   *  shipping builds (only compiled when -DIPLUG_SENTRY_WATCHDOG_DEBUG is
   *  set). NOT thread-safe vs concurrent Ticks — call from a quiescent
   *  plugin instance only.
   */
  void DebugForceStall(SlotHandle handle);
#endif

#else // !IPLUG_USE_SENTRY — every entry point is a no-op.

  inline SlotHandle Register() { return SlotHandle{0, 0}; }
  inline void Unregister(SlotHandle /*handle*/) {}
  inline void Tick(SlotHandle /*handle*/, int32_t /*blockSize*/,
                   double /*sampleRate*/, bool /*renderingOffline*/) {}
  inline void Start() {}
  inline void Stop() {}

#endif // IPLUG_USE_SENTRY

} // namespace watchdog
} // namespace sentry
} // namespace iplug
