/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#include "IPlugSentryWatchdog.h"

// All non-trivial code is gated behind IPLUG_USE_SENTRY. The OFF build never
// reaches anything in this TU because the header's #else branch provides
// inline no-op definitions for every entry point — the OFF-branch stub at
// the bottom keeps the file as a self-contained empty object so the CMake
// surface stays uniform.
#ifdef IPLUG_USE_SENTRY

  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdint>
  #include <cstdlib>
  #include <cstring>
  #include <future>
  #include <mutex>
  #include <thread>

  #include <sentry.h>

  #if defined(__APPLE__)
    #include <mach/mach.h>
    #include <pthread.h>
  #endif

namespace iplug
{
namespace sentry
{
namespace watchdog
{

// Forward declaration of the macOS capture entry point (defined in the .mm).
// MUST be at named-namespace scope (not anonymous) so the linker resolves
// it to the .mm's definition rather than an internal-linkage stub.
#if defined(__APPLE__)
void CaptureStall_macOS(uint32_t audioMachThread,
                        std::chrono::milliseconds stallMs,
                        int32_t blockSize,
                        double sampleRate);
#endif

// Namespace-scope lock-freeness asserts. Better here than inside Start()
// so they fire at compile time on every translation unit regardless of
// whether Start is ever reached.
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "watchdog requires lock-free 64-bit atomics");
static_assert(std::atomic<double>::is_always_lock_free,
              "watchdog requires lock-free atomic<double> on the audio thread");
static_assert(std::atomic<int32_t>::is_always_lock_free,
              "watchdog requires lock-free 32-bit atomics");
static_assert(std::atomic<bool>::is_always_lock_free,
              "watchdog requires lock-free atomic<bool>");

// ---------------------------------------------------------------------------
// HeartbeatSlot layout.
//
// One cache line per slot to keep audio-thread stores from contending with
// the watchdog thread's reads on adjacent slots. Members split into two
// regions:
//   - Atomic, touched by audio (relaxed/release) and watchdog (relaxed/acq)
//   - Plain, touched ONLY by the watchdog thread (no atomicity needed,
//     except that the watchdog's access happens-after the audio thread's
//     publication via the inUse/generation acquire-release pair)
// ---------------------------------------------------------------------------
struct alignas(64) HeartbeatSlot
{
  // Audio-thread writes (relaxed), watchdog-thread reads (relaxed/acquire).
  std::atomic<uint64_t> tick{0};
  std::atomic<int32_t>  blockSize{0};
  std::atomic<double>   sampleRate{0.0};
  // Release on the audio side so the watchdog's acquire load pairs with it
  // and observes a coherent view of (tick, blockSize, sampleRate). Without
  // the acquire-release pair the watchdog can read stale offline=false and
  // a stale tick counter during host freeze/bounce and fire a false stall.
  std::atomic<bool>     renderingOffline{false};

  // Slot allocator state. Release-store on publish so the next claimer's
  // acquire-load of inUse=true also observes the freshly-zeroed body.
  std::atomic<bool>     inUse{false};

  // Generation counter. Bumped on Unregister + on the publishing end of
  // Register so Tick's relaxed cross-check catches stale handles.
  // Initialised to 1 in BSS so the first claim returns gen>=2 and the
  // zero-gen sentinel is never accidentally issued.
  std::atomic<uint16_t> generation{1};

  // Optional capture of the audio thread's mach_port — set ONCE per slot
  // lifetime, on the cold path of Tick. Uses pthread_mach_thread_np()
  // semantics (no send-right refcount bump, no deallocate needed).
#if defined(__APPLE__)
  std::atomic<uint32_t> audioMachThread{0}; // mach_port_t is unsigned int
#endif

  // ---------- Watchdog-thread-only fields. No atomicity required. ----------
  uint64_t lastObservedTick = 0;
  std::chrono::steady_clock::time_point lastChangeAt{};
  bool hasEverTicked = false;
  bool stallEpisodeActive = false;
  int  recoveryTickCount = 0;     // consecutive advancing checks since stall
  bool sawOfflineThisEpisode = false; // freeze/bounce within current window
};

// Tuning constants. All overridable via -D at build time so plugin builders
// can adjust without forking iPlug2.
#ifndef IPLUG_WATCHDOG_SLOT_COUNT
  #define IPLUG_WATCHDOG_SLOT_COUNT 128
#endif
#ifndef IPLUG_WATCHDOG_CHECK_INTERVAL_MS
  #define IPLUG_WATCHDOG_CHECK_INTERVAL_MS 50
#endif
#ifndef IPLUG_WATCHDOG_MIN_STALL_MS
  #define IPLUG_WATCHDOG_MIN_STALL_MS 200
#endif
#ifndef IPLUG_WATCHDOG_STALL_BLOCK_MULTIPLIER
  #define IPLUG_WATCHDOG_STALL_BLOCK_MULTIPLIER 50
#endif
#ifndef IPLUG_WATCHDOG_MAX_EVENTS_PER_SESSION
  #define IPLUG_WATCHDOG_MAX_EVENTS_PER_SESSION 20
#endif
#ifndef IPLUG_WATCHDOG_RECOVERY_CHECKS
  #define IPLUG_WATCHDOG_RECOVERY_CHECKS 3
#endif

namespace
{
  constexpr size_t kSlotCount = IPLUG_WATCHDOG_SLOT_COUNT;
  constexpr std::chrono::milliseconds kCheckInterval{IPLUG_WATCHDOG_CHECK_INTERVAL_MS};
  constexpr std::chrono::milliseconds kMinStallThreshold{IPLUG_WATCHDOG_MIN_STALL_MS};
  constexpr int kStallBlockMultiplier = IPLUG_WATCHDOG_STALL_BLOCK_MULTIPLIER;
  constexpr size_t kMaxHangEventsPerSession = IPLUG_WATCHDOG_MAX_EVENTS_PER_SESSION;
  constexpr int kRecoveryChecks = IPLUG_WATCHDOG_RECOVERY_CHECKS;
  constexpr std::chrono::milliseconds kStopJoinTimeout{200};

  static_assert(kSlotCount < 65536,
                "SlotHandle::idx is uint16_t — kSlotCount must fit");

  HeartbeatSlot gSlots[kSlotCount];

  std::mutex gAllocMutex;

  // Worker thread + shutdown signalling.
  std::thread gThread;
  std::once_flag gStartOnce;
  std::atomic<bool> gStopRequested{false};
  std::atomic<bool> gThreadAlive{false};
  std::atomic<bool> gShuttingDown{false};
  std::mutex gWakeMutex;
  std::condition_variable gWakeCv;

  // Per-session emit budget. Stops a thrashing host from flooding Sentry.
  std::atomic<size_t> gHangEventCount{0};

  // High-water mark of claimed slot indices — lets the watchdog skip the
  // unused tail of the table on hosts with few plugin instances.
  std::atomic<size_t> gHighestClaimedIdx{0};

  bool HandleIsZero(SlotHandle h) { return h.idx == 0 && h.gen == 0; }

  // ---------------------------------------------------------------------------
  // Capture entry points.
  // ---------------------------------------------------------------------------

#if defined(_WIN32)
  // Windows: build a sentry event with a synthetic thread + stacktrace from
  // the watchdog thread's own capture. The Crashpad backend would attach
  // all threads on a true CRASHPAD_SIMULATE_CRASH, but that macro lives
  // inside crashpad headers not exposed to consumers; we use the public
  // sentry-native API and ship a hand-built event with the wedged thread's
  // PC via StackWalk64 in a follow-up. For now we ship an event tagged
  // event_type=audio_thread_hang so dashboards alert and triage can pull
  // local minidumps. TODO(A-5-fixup): port-in the SuspendThread +
  // CaptureStackBackTrace path mirroring the macOS unsandboxed branch so
  // Windows events carry an actual stacktrace.
  void CaptureStall_Windows(HeartbeatSlot& /*slot*/,
                            std::chrono::milliseconds stallMs)
  {
    sentry_value_t event = sentry_value_new_message_event(
      SENTRY_LEVEL_WARNING, "audio-thread-hang",
      "iPlug2 watchdog detected stalled ProcessBlock");
    sentry_value_set_by_key(event, "logger",
      sentry_value_new_string("iplug.watchdog"));

    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "event_type",
      sentry_value_new_string("audio_thread_hang"));
    char durBuf[32];
    std::snprintf(durBuf, sizeof(durBuf), "%lld", (long long) stallMs.count());
    sentry_value_set_by_key(tags, "stall_duration_ms",
      sentry_value_new_string(durBuf));
    sentry_value_set_by_key(event, "tags", tags);

    // Also stash duration as a numeric extra so Discover numeric filters
    // (stall_duration_ms:>500) actually work.
    sentry_value_t extra = sentry_value_new_object();
    sentry_value_set_by_key(extra, "stall_duration_ms",
      sentry_value_new_int32((int32_t) stallMs.count()));
    sentry_value_set_by_key(event, "extra", extra);

    sentry_capture_event(event);
  }
#endif

  void CaptureStall(HeartbeatSlot& slot, std::chrono::milliseconds stallMs)
  {
    // Hard cap per session to avoid flooding Sentry on a thrashing host.
    if (gHangEventCount.fetch_add(1, std::memory_order_relaxed)
          >= kMaxHangEventsPerSession)
      return;
    // Bail if shutdown started — sentry-native transport may be torn down.
    if (gShuttingDown.load(std::memory_order_relaxed)) return;

#if defined(_WIN32)
    CaptureStall_Windows(slot, stallMs);
#elif defined(__APPLE__)
    const uint32_t machPort = slot.audioMachThread.load(std::memory_order_relaxed);
    const int32_t  bs       = slot.blockSize.load(std::memory_order_relaxed);
    const double   sr       = slot.sampleRate.load(std::memory_order_relaxed);
    CaptureStall_macOS(machPort, stallMs, bs, sr);
#else
    (void) slot;
    (void) stallMs;
#endif
  }

  // ---------------------------------------------------------------------------
  // Watchdog thread main.
  // ---------------------------------------------------------------------------

  std::chrono::milliseconds StallThresholdFor(int32_t blockSize, double sampleRate)
  {
    if (blockSize <= 0 || sampleRate <= 0.0) return kMinStallThreshold;
    // Clamp blockSize to a sane ceiling so a malformed host value can't
    // overflow the int64_t millisecond cast. 65536 frames is well above
    // any real-world DAW block.
    const int32_t bs = blockSize > 65536 ? 65536 : blockSize;
    const double seconds = (double) kStallBlockMultiplier * (double) bs / sampleRate;
    if (seconds <= 0.0) return kMinStallThreshold;
    const double ms = seconds * 1000.0;
    if (ms > 60000.0) return std::chrono::milliseconds{60000}; // 60s ceiling
    const std::chrono::milliseconds blockBased{(int64_t) ms};
    return (blockBased > kMinStallThreshold) ? blockBased : kMinStallThreshold;
  }

  void CheckSlot(HeartbeatSlot& slot,
                 std::chrono::steady_clock::time_point now)
  {
    // Snapshot generation BEFORE reading the body so we can detect mid-read
    // churn (Unregister + Register on another thread).
    const uint16_t genBefore = slot.generation.load(std::memory_order_acquire);

    // acquire so we see the publisher's initial zeroing + field writes
    // before reading any other slot field.
    if (!slot.inUse.load(std::memory_order_acquire)) return;

    // acquire-load on the audio thread's release-store of renderingOffline
    // pairs the watchdog's view of (tick, blockSize, sampleRate).
    const bool offline = slot.renderingOffline.load(std::memory_order_acquire);
    if (offline) {
      slot.stallEpisodeActive = false;
      slot.sawOfflineThisEpisode = true;
      slot.lastObservedTick = slot.tick.load(std::memory_order_relaxed);
      slot.lastChangeAt = now;
      slot.recoveryTickCount = 0;
      return;
    }

    const uint64_t currentTick = slot.tick.load(std::memory_order_relaxed);

    // Re-check generation after the body read — if it churned, the slot
    // was freed + potentially re-claimed mid-iteration. Drop this pass and
    // reset our cached state so next iteration starts fresh.
    const uint16_t genAfter = slot.generation.load(std::memory_order_acquire);
    if (genBefore != genAfter)
    {
      slot.lastObservedTick = currentTick;
      slot.lastChangeAt = now;
      slot.hasEverTicked = false;
      slot.stallEpisodeActive = false;
      slot.recoveryTickCount = 0;
      slot.sawOfflineThisEpisode = false;
      return;
    }

    if (currentTick != slot.lastObservedTick)
    {
      slot.lastObservedTick = currentTick;
      slot.lastChangeAt = now;
      slot.hasEverTicked = true;
      // N-consecutive-recovery dedupe: only clear the episode latch after
      // kRecoveryChecks successful tick advances, so a stall that recovers
      // for one block then re-stalls counts as one episode.
      if (slot.stallEpisodeActive)
      {
        if (++slot.recoveryTickCount >= kRecoveryChecks)
        {
          slot.stallEpisodeActive = false;
          slot.sawOfflineThisEpisode = false;
          slot.recoveryTickCount = 0;
        }
      }
      else
      {
        slot.recoveryTickCount = 0;
      }
      return;
    }

    // Tick has not moved.
    if (!slot.hasEverTicked) return;
    if (slot.stallEpisodeActive) return;
    // If we ever saw offline render within the current window, give the
    // engine a wide berth before firing — freeze/bounce can pause the
    // audio thread for seconds. Reset the window after a tick advances.
    if (slot.sawOfflineThisEpisode) return;

    const auto threshold = StallThresholdFor(
      slot.blockSize.load(std::memory_order_relaxed),
      slot.sampleRate.load(std::memory_order_relaxed));

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - slot.lastChangeAt);
    if (elapsed < threshold) return;

    slot.stallEpisodeActive = true;
    slot.recoveryTickCount = 0;
    CaptureStall(slot, elapsed);
  }

  void ThreadMain()
  {
    while (!gStopRequested.load(std::memory_order_relaxed))
    {
      try
      {
        // Sleep interruptibly so Stop() wakes us immediately. Predicate is
        // checked under the cv mutex; a relaxed re-load at the top of the
        // loop catches the post-wait state.
        {
          std::unique_lock<std::mutex> lock(gWakeMutex);
          gWakeCv.wait_for(lock, kCheckInterval, []{
            return gStopRequested.load(std::memory_order_relaxed);
          });
        }
        if (gStopRequested.load(std::memory_order_relaxed)) break;

        const auto now = std::chrono::steady_clock::now();
        const size_t high = gHighestClaimedIdx.load(std::memory_order_relaxed);
        // gHighestClaimedIdx is 1-based; we sweep slots [0, high).
        for (size_t i = 0; i < high && i < kSlotCount; ++i)
        {
          if (gStopRequested.load(std::memory_order_relaxed)) break;
          CheckSlot(gSlots[i], now);
        }
      }
      catch (...)
      {
        // Defense-in-depth: one rogue capture (sentry OOM, libunwind throw,
        // anything) must not silently kill the watchdog. Swallow and loop.
      }
    }
    gThreadAlive.store(false, std::memory_order_release);
  }

  // atexit hook — registered from Start() AFTER sentry-native's own
  // atexit registration so this runs FIRST at exit (LIFO). Bounded join.
  void AtExitStop()
  {
    gShuttingDown.store(true, std::memory_order_relaxed);
    Stop();
  }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

SlotHandle Register()
{
  std::lock_guard<std::mutex> lock(gAllocMutex);
  for (size_t i = 0; i < kSlotCount; ++i)
  {
    HeartbeatSlot& slot = gSlots[i];
    bool expected = false;
    // Reserve the slot first with a relaxed CAS — we'll publish with a
    // release store at the end after zeroing the body.
    if (!slot.inUse.compare_exchange_strong(
          expected, true,
          std::memory_order_acq_rel, std::memory_order_relaxed))
      continue;

    // Zero the audio-facing body BEFORE publishing the slot's generation
    // to the audio side, so any racing Tick observes a coherent slot.
    slot.tick.store(0, std::memory_order_relaxed);
    slot.blockSize.store(0, std::memory_order_relaxed);
    slot.sampleRate.store(0.0, std::memory_order_relaxed);
    slot.renderingOffline.store(false, std::memory_order_relaxed);
#if defined(__APPLE__)
    slot.audioMachThread.store(0, std::memory_order_relaxed);
#endif
    slot.lastObservedTick = 0;
    slot.lastChangeAt = std::chrono::steady_clock::now();
    slot.hasEverTicked = false;
    slot.stallEpisodeActive = false;
    slot.recoveryTickCount = 0;
    slot.sawOfflineThisEpisode = false;

    // Bump generation last with release ordering so audio-side Tick's
    // acquire-load of generation observes the freshly-zeroed body.
    // Loop deterministically skips the zero sentinel (handles wrap from
    // 65535 -> 0 cleanly even though we initialise generation to 1).
    uint16_t gen;
    do {
      gen = (uint16_t)(slot.generation.fetch_add(1, std::memory_order_acq_rel) + 1);
    } while (gen == 0);

    const uint16_t handleIdx = (uint16_t)(i + 1);
    // Track high-water mark so the watchdog can skip the unused tail.
    size_t prevHigh = gHighestClaimedIdx.load(std::memory_order_relaxed);
    while (prevHigh < (size_t) handleIdx &&
           !gHighestClaimedIdx.compare_exchange_weak(
             prevHigh, (size_t) handleIdx,
             std::memory_order_relaxed, std::memory_order_relaxed)) {}

    return SlotHandle{ handleIdx, gen };
  }
  return SlotHandle{0, 0};
}

void Unregister(SlotHandle handle)
{
  if (HandleIsZero(handle)) return;
  if (handle.idx == 0 || handle.idx > kSlotCount) return;

  std::lock_guard<std::mutex> lock(gAllocMutex);
  HeartbeatSlot& slot = gSlots[handle.idx - 1];

#if defined(__APPLE__)
  // pthread_mach_thread_np() semantics: the port name doesn't hold a send
  // right refcount so there's nothing to deallocate. Just clear it.
  slot.audioMachThread.store(0, std::memory_order_relaxed);
#endif

  // Bump generation FIRST (release) so any in-flight Tick from the old
  // owner that already passed the inUse gate fails its gen cross-check
  // before mutating any field.
  uint16_t gen;
  do {
    gen = (uint16_t)(slot.generation.fetch_add(1, std::memory_order_acq_rel) + 1);
  } while (gen == 0);

  // Release inUse last so a watchdog observing inUse=true (via acquire)
  // sees the freshly-bumped generation and treats the slot as freed on
  // its next iteration regardless.
  slot.inUse.store(false, std::memory_order_release);
}

void Tick(SlotHandle handle, int32_t blockSize, double sampleRate, bool renderingOffline)
{
  // Fast-path bail: zero handle = unregistered or full-table.
  if (handle.idx == 0) return;
  if (handle.idx > kSlotCount) return; // defensive
  HeartbeatSlot& slot = gSlots[handle.idx - 1];

  // Generation cross-check. One relaxed load + compare — ~1ns on modern
  // CPUs and perfectly predicted in steady state. Prevents cross-instance
  // tick smearing when slot.idx has been re-claimed by another instance
  // after our Unregister hasn't yet caught up on this thread.
  const uint16_t curGen = slot.generation.load(std::memory_order_relaxed);
  if (curGen != handle.gen) return;

  slot.tick.fetch_add(1, std::memory_order_relaxed);
  slot.blockSize.store(blockSize, std::memory_order_relaxed);
  slot.sampleRate.store(sampleRate, std::memory_order_relaxed);
  // Release on the offline flag so the watchdog's acquire-load of it
  // sees a coherent view of the prior three stores during freeze/bounce.
  slot.renderingOffline.store(renderingOffline, std::memory_order_release);

#if defined(__APPLE__)
  // Record the audio thread's mach port the first time. pthread_mach_thread_np
  // returns the thread's port name WITHOUT bumping the send-right refcount,
  // so there's no kernel allocation, no syscall, and no port leak.
  // (mach_thread_self() WOULD do all three — we deliberately avoid it on
  // the audio thread.)
  const uint32_t prev = slot.audioMachThread.load(std::memory_order_relaxed);
  if (prev == 0)
  {
    const mach_port_t self = pthread_mach_thread_np(pthread_self());
    slot.audioMachThread.store(static_cast<uint32_t>(self),
                               std::memory_order_relaxed);
  }
#endif
}

void Start()
{
  std::call_once(gStartOnce, []() {
    // Runtime kill switch. Read once — env reads are syscalls.
    if (const char* off = std::getenv("PLUGINMAKER_WATCHDOG_DISABLE"))
    {
      if (*off && *off != '0') return;
    }

    gStopRequested.store(false, std::memory_order_relaxed);
    gShuttingDown.store(false, std::memory_order_relaxed);
    gThreadAlive.store(true, std::memory_order_relaxed);

    try
    {
      gThread = std::thread(ThreadMain);
    }
    catch (...)
    {
      // OOM / thread-limit. Failed once, failed forever — singleton.
      gThreadAlive.store(false, std::memory_order_relaxed);
      return;
    }

    // Register the shutdown hook AFTER sentry-native finished its own
    // atexit registration (sentry_init ran before us). atexit handlers
    // fire in LIFO order, so ours runs FIRST at exit — we stop the
    // producer before the consumer's transport tears down.
    std::atexit(&AtExitStop);
  });
}

void Stop()
{
  gStopRequested.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(gWakeMutex);
  }
  gWakeCv.notify_all();

  if (!gThread.joinable()) return;

  // Bounded join. We MUST NOT block DLL unload / atexit indefinitely if
  // the worker is stuck inside a sentry capture against a torn-down
  // transport. Race join() against a timeout via std::async; on miss,
  // detach() so the process can exit.
  auto fut = std::async(std::launch::async, []() {
    if (gThread.joinable()) gThread.join();
  });
  if (fut.wait_for(kStopJoinTimeout) != std::future_status::ready)
  {
    if (gThread.joinable()) gThread.detach();
  }
}

#ifdef IPLUG_SENTRY_WATCHDOG_DEBUG
void DebugForceStall(SlotHandle handle)
{
  if (HandleIsZero(handle)) return;
  if (handle.idx == 0 || handle.idx > kSlotCount) return;
  HeartbeatSlot& slot = gSlots[handle.idx - 1];
  // Pretend last tick change was 10 seconds ago — next CheckSlot iteration
  // will fire a stall event. Not thread-safe vs concurrent Ticks; callers
  // must quiesce the plugin instance first.
  slot.hasEverTicked = true;
  slot.lastChangeAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  slot.stallEpisodeActive = false;
  slot.sawOfflineThisEpisode = false;
}
#endif

} // namespace watchdog
} // namespace sentry
} // namespace iplug

#else // !IPLUG_USE_SENTRY

// Intentionally empty TU. The header's #else branch already provides inline
// no-op definitions of every entry point, so callers (IPlugProcessor.cpp)
// see usable definitions at every include site without linking any extra
// symbol. Mirrors IPlugSentry.cpp's OFF-build structure.

#endif // IPLUG_USE_SENTRY
