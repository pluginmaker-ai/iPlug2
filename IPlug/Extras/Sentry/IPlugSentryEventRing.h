/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file
 * @brief SPSC lock-free event ring + drain thread for iPlug2 plugins
 *        (Surface A — A-6).
 *
 * The audio thread (and the Faust per-stage probe seam — Stream G) pushes
 * fixed-size event records into a single-producer / single-consumer ring.
 * A drain thread consumes the ring and formats each record into a bounded
 * circular text buffer (`audio_events.log`). On the next hang or crash the
 * Sentry watchdog + before_send hooks attach the tail of that text buffer
 * to the outgoing event so the dashboard sees exactly what the audio
 * subsystem did in the seconds before failure.
 *
 * Why a fixed-size 32-byte record
 * -------------------------------
 *   Audio-thread allocations are forbidden. A POD record with no embedded
 *   pointers means push() is a memcpy of 32 bytes into a pre-allocated ring
 *   slot, followed by a single release store of the writer index. The
 *   record is small enough that 4096 slots fits in 128 KiB — one cache
 *   line per pair of records and well inside L2 on every supported host.
 *
 * Why rigtorp-style SPSC
 * ----------------------
 *   The producer is the audio thread (any of N IPlugProcessor instances —
 *   they each push to the same per-DLL ring; see "shared ring" below) and
 *   the consumer is a single dedicated drain thread. There are never two
 *   concurrent producers or two concurrent consumers, so the cheaper
 *   rigtorp SPSC scheme suffices: one atomic writer index, one atomic
 *   reader index, no per-slot atomic state word. The push and pop paths
 *   touch one cache line each (writer or reader index), with the slot
 *   payload reached via plain memcpy.
 *
 *   Multiple plugin instances on the audio thread are still "single
 *   producer" because the host serialises ProcessBlock calls within a
 *   process — the audio thread is one thread of execution that fans
 *   across instances. Cross-instance Push() calls are therefore strictly
 *   ordered. Faust probe callbacks fire from inside ProcessBlock, on the
 *   same thread, so they reuse the same SPSC discipline.
 *
 *   The "audio thread is one OS thread" property does NOT hold across
 *   distinct top-level audio devices in the same host (a power user with
 *   two audio interfaces running independent ProcessBlock pumps). That
 *   would race the writer index and corrupt the ring. We accept that
 *   constraint for v1 — the failure mode is "dropped events / dashboard
 *   garbage on multi-device setups" not "crash". A later upgrade to an
 *   MPSC ring (rigtorp or moodycamel) addresses this; out of scope here.
 *
 * Drop policy
 * -----------
 *   Ring full → overwrite=false. The push returns false and the lost
 *   event is accounted for by a relaxed counter. When the drain thread
 *   sees the counter advance it emits a single RING_OVERFLOW synthesised
 *   record into the rendered text buffer (so the dashboard sees "we
 *   dropped N events between t0 and t1") and resets the counter. The
 *   audio thread NEVER blocks on a full ring.
 *
 * Stage-name interning
 * --------------------
 *   The Faust probe seam wants to identify which DSP stage misbehaved
 *   ("compressor_1", "reverb_tail"). Storing strings in the 32-byte
 *   record is impossible; storing pointers risks dangling references if
 *   the caller frees the string. We solve this with a fixed-capacity
 *   intern table sized at 64 entries. Internment is one-shot: the first
 *   time a given name is seen it's copied into a flat char arena, the
 *   address is stable for the process lifetime, and a small `stage_id`
 *   (uint32) is returned. The record stores only the id. The drain side
 *   resolves id → name when rendering the human-readable log line.
 *
 *   Intern on the audio thread? Yes — but the intern path is
 *   `lookup → if found return id; else cold-path append`. The lookup is
 *   a linear scan of at most 64 small char buffers (single cache line
 *   each) which is cheaper than a hash table for n ≤ 64. The cold-path
 *   append uses a try-lock; if the lock is contested, the audio thread
 *   bails to a fallback id of 0 ("unknown") rather than waiting. After
 *   the first warm-up calls the table is fully populated and the audio
 *   thread only ever hits the read-only fast path.
 *
 * Drain thread
 * ------------
 *   ONE per DLL, NOT the watchdog thread (separation of concerns: the
 *   watchdog must remain a tight tick-and-detect loop; mixing event
 *   formatting in would risk widening its wake budget). Started lazily
 *   from `iplug::sentry::Init()` AFTER `watchdog::Start()` returns so
 *   that init order is: sentinel → sentry_init → pending-dump scan →
 *   watchdog start → drain start. Joined via atexit with a bounded
 *   200ms timeout (same pattern as the watchdog).
 *
 * Compile-time gating
 * -------------------
 *   When IPLUG_USE_SENTRY is NOT defined, every entry point is an inline
 *   empty function. The Push() return is hardcoded `false`, intern is
 *   hardcoded `0`. Callers compile + link cleanly with no extra symbol
 *   surface, the OFF build never reaches the .cpp.
 */

namespace iplug
{
namespace sentry
{
namespace eventring
{

  /** Event taxonomy. Each enumerator MUST be < 65536 to fit in `kind`. */
  enum EventKind : uint16_t
  {
    EVENT_KIND_UNKNOWN          = 0,

    // Audio-thread edge detectors (see IPlugSentryAudioDetectors.h).
    EVENT_KIND_BLOCK_CPU_OVER_80 = 1,
    EVENT_KIND_NAN_OUTPUT        = 2,
    EVENT_KIND_INF_OUTPUT        = 3,

    // Faust per-stage probe seam (Stream G).
    EVENT_KIND_PROBE_NAN         = 10,
    EVENT_KIND_PROBE_SILENCE     = 11,
    EVENT_KIND_PROBE_CPU_OVER    = 12,

    // Synthesised by the drain thread when the ring overflows.
    EVENT_KIND_RING_OVERFLOW     = 100,
  };

  /** Fixed 32-byte event record. Trivially copyable POD. No heap, no
   *  embedded pointers. Layout is explicit so future cross-build readers
   *  (e.g. crash-time forensics from a minidump) can decode it without
   *  the producing TU's headers.
   *
   *  Field order is chosen so the natural alignment of each field is
   *  satisfied without padding, keeping the record exactly 32 bytes on
   *  every supported ABI (LLP64/LP64, x86-64, arm64).
   */
  struct EventRecord
  {
    uint64_t timestampNs;   // monotonic timestamp, source-relative
    uint16_t kind;          // EventKind (uint16 for compactness)
    int16_t  channel;       // -1 when not applicable
    float    value;         // load_pct (CPU events), NaN/Inf marker (1.0f), etc.
    uint32_t stageId;       // 0 for non-probe events, else intern id
    uint32_t reserved0;     // future use; MUST be zero on push
    uint64_t reserved1;     // future use; MUST be zero on push
  };
  static_assert(sizeof(EventRecord) == 32,
                "EventRecord must be exactly 32 bytes — ABI contract for crash forensics");

#ifdef IPLUG_USE_SENTRY

  /** Push an event into the per-DLL SPSC ring. AUDIO-THREAD SAFE.
   *
   *  Wait-free in the steady state — touches one ring slot (memcpy 32B)
   *  and one atomic store-release on the writer index. No allocations,
   *  no syscalls, no locks. Returns true if the event was enqueued,
   *  false if the ring was full (lost event accounted into the overflow
   *  counter; drain thread will synthesise a RING_OVERFLOW record).
   *
   *  Callers MUST zero the `reserved0`/`reserved1` fields. The Push
   *  implementation does not scrub them — the wire format treats them
   *  as caller-provided and a future drain-side decoder may key on
   *  them being zero today.
   */
  bool Push(const EventRecord& rec) noexcept;

  /** Intern a stage name and return its stable id. Audio-thread safe.
   *
   *  Returns 0 when:
   *    - The table is full (capped at 64 entries) AND the name is new.
   *    - The cold-path append lock is contested by a concurrent intern
   *      (only possible across distinct audio threads in multi-device
   *      hosts — see header file comment).
   *    - `name` is null or empty.
   *
   *  Successful interns produce a non-zero id. Once an id is issued for
   *  a given name it is stable for the process lifetime.
   *
   *  The audio-thread fast path is a linear scan of ≤ 64 short C strings.
   *  Once the table is warmed up by the first few probe events this
   *  scan never reaches the cold-path append branch again.
   */
  uint32_t InternStageName(const char* name) noexcept;

  /** Start the per-DLL drain thread. Idempotent.
   *
   *  Called from `iplug::sentry::Init()` AFTER `watchdog::Start()` so the
   *  ring is ready as soon as the watchdog can fire a hang event. Reads
   *  the `PLUGINMAKER_EVENT_RING_DISABLE` env var and bails if set —
   *  runtime kill switch matching the watchdog pattern. Registers an
   *  atexit() hook that calls StopDrainThread() with a bounded join
   *  timeout (200ms ceiling, matches watchdog).
   *
   *  Safe to call from any thread but NOT from the audio thread
   *  (allocates the ring + spawns std::thread).
   */
  void StartDrainThread();

  /** Returns true once StartDrainThread has spawned the drain thread + the
   *  attachment path has been resolved. Audio-thread Push paths use this
   *  as a fast-path gate so they silently drop records BEFORE init
   *  completes (consent denied, no DSN, init still in flight, etc.) —
   *  prevents the ring from filling on legitimate probes that fire from
   *  ProcessBlock during the first ~seconds of plugin life.
   *
   *  One relaxed atomic load — RT-safe to call from the audio thread.
   */
  bool IsDrainRunning() noexcept;

  /** Monotonic nanosecond timestamp from the same clock source the audio
   *  detectors use (mach_absolute_time on macOS, QueryPerformanceCounter
   *  on Windows). Exposed publicly so the probe seam TU records on the
   *  same timeline as ProcessBlock detectors and the rendered
   *  audio_events.log sorts coherently across record sources.
   *
   *  RT-safe — no syscall on Mac arm64+x64 or Win10+ x64.
   */
  uint64_t NowNs() noexcept;

  /** Request the drain thread to exit + join (bounded). Idempotent.
   *
   *  Called from the atexit() hook or by tests. Signals shutdown, wakes
   *  the drain thread, joins with a 200ms ceiling. If the join misses
   *  the ceiling the thread is detached — process exit MUST NOT block
   *  on a wedged drain.
   */
  void StopDrainThread();

  /** Copy the most recent rendered text into `outBuf`. Returns the number
   *  of bytes written (≤ `outBufCapacity`).
   *
   *  Called by the watchdog (hang capture) and the IPlugSentry
   *  before_send hook (crash capture) when assembling an event. Cold-
   *  path operation — takes the rendered text buffer's mutex briefly to
   *  copy out the tail. Safe to call from non-audio threads.
   *
   *  When `outBuf` is null or `outBufCapacity` is 0 the call returns 0
   *  without copying anything. If the rendered buffer has more data
   *  than fits, only the trailing `outBufCapacity` bytes are copied
   *  (we want the moments closest to the failure, not the oldest).
   */
  std::size_t CopyRenderedTail(char* outBuf, std::size_t outBufCapacity) noexcept;

#else // !IPLUG_USE_SENTRY — every entry point is a no-op.

  inline bool Push(const EventRecord& /*rec*/) noexcept { return false; }
  inline uint32_t InternStageName(const char* /*name*/) noexcept { return 0; }
  inline bool IsDrainRunning() noexcept { return false; }
  inline uint64_t NowNs() noexcept { return 0; }
  inline void StartDrainThread() {}
  inline void StopDrainThread() {}
  inline std::size_t CopyRenderedTail(char* /*outBuf*/,
                                      std::size_t /*outBufCapacity*/) noexcept
  {
    return 0;
  }

#endif // IPLUG_USE_SENTRY

} // namespace eventring
} // namespace sentry
} // namespace iplug
