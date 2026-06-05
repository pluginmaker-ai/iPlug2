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
  #include <cstdio>
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

  #if defined(_WIN32)
    // <windows.h> must precede <dbghelp.h>. Auto-link the dbghelp import lib
    // here so MSBuild picks it up without us having to touch iPlug2OOS props.
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
    #include <windows.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
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

  // Optional capture of the audio thread's Win32 thread ID — set ONCE per
  // slot lifetime on the cold path of Tick. GetCurrentThreadId() is a TEB
  // read (FS:[0x48] on x64, ~1-2 cycles, no syscall, RT-safe). We store
  // the tid (NOT a HANDLE) so there is nothing to close on Unregister and
  // the audio thread never needs to call OpenThread (syscall). The
  // watchdog opens a fresh limited-rights handle on demand when it needs
  // to walk the stack. We also snapshot the kernel-side CreationTime once
  // so the watchdog can detect TID reuse — Windows recycles thread IDs
  // aggressively once a thread exits, and the saved CreationTime is the
  // stable per-thread identity that survives recycling.
#if defined(_WIN32)
  std::atomic<uint32_t> audioWinThreadId{0};
  // Two halves of a FILETIME captured via GetThreadTimes; published with
  // release after the tid is set so the watchdog's acquire load of the
  // tid serialises observability of these.
  std::atomic<uint32_t> audioWinCreationTimeLow{0};
  std::atomic<uint32_t> audioWinCreationTimeHigh{0};
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
#ifndef IPLUG_WATCHDOG_MAX_STACK_FRAMES
  #define IPLUG_WATCHDOG_MAX_STACK_FRAMES 64
#endif
#ifndef IPLUG_WATCHDOG_MAX_DEBUG_IMAGES
  #define IPLUG_WATCHDOG_MAX_DEBUG_IMAGES 64
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
#if defined(_WIN32)
  constexpr size_t kMaxStackFrames = IPLUG_WATCHDOG_MAX_STACK_FRAMES;
  constexpr size_t kMaxDebugImages = IPLUG_WATCHDOG_MAX_DEBUG_IMAGES;
  // Cap retries on a transient ResumeThread failure so we cannot spin forever.
  constexpr int    kResumeRetryCount = 4;
  // DbgHelp mutex timeout — we'd rather emit a no-frames event than block the
  // watchdog forever if a sibling DLL is monopolising DbgHelp.
  constexpr DWORD  kDbgHelpMutexTimeoutMs = 500;
#endif

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

#if defined(_WIN32)
  // Cached watchdog thread tid so the self-suspend guard cannot be fooled
  // by impersonation or APC-driven identity changes.
  std::atomic<uint32_t> gWatchdogTid{0};
#endif

  bool HandleIsZero(SlotHandle h) { return h.idx == 0 && h.gen == 0; }

  // ---------------------------------------------------------------------------
  // Capture entry points.
  // ---------------------------------------------------------------------------

#if defined(_WIN32)

  // DbgHelp symbol init. SymInitialize is not thread-safe and is one-shot
  // per process — guard with call_once. We also tolerate the case where
  // another DLL in the same process beat us to it (ERROR_INVALID_PARAMETER
  // / 87) — in that case we still SymRefreshModuleList so we have a sane
  // module table regardless of how the original SymInitialize was invoked.
  std::once_flag gSymInitOnce;
  std::atomic<bool> gSymInitOk{false};

  // Process-wide named mutex serialising DbgHelp calls across multiple
  // iPlug2-based plugin DLLs in the same host. DbgHelp is documented as
  // single-threaded per process; our per-DLL `gThread` only serialises
  // within ONE DLL, so without this mutex two iPlug2 plugins in the same
  // host can race DbgHelp's internal state. Created on first use.
  std::mutex gDbgHelpMutexCreate;
  std::atomic<HANDLE> gDbgHelpMutex{nullptr};

  HANDLE EnsureDbgHelpMutex()
  {
    HANDLE existing = gDbgHelpMutex.load(std::memory_order_acquire);
    if (existing) return existing;
    std::lock_guard<std::mutex> lk(gDbgHelpMutexCreate);
    existing = gDbgHelpMutex.load(std::memory_order_acquire);
    if (existing) return existing;
    // `Local\` namespace = current session, no admin required.
    HANDLE h = CreateMutexW(NULL, FALSE, L"Local\\iPlug2-DbgHelp-Lock");
    if (h == NULL) return NULL;
    gDbgHelpMutex.store(h, std::memory_order_release);
    return h;
  }

  // RAII lock wrapper around the named mutex. WaitForSingleObject with a
  // bounded timeout so we cannot wedge the watchdog if a sibling DLL is
  // monopolising DbgHelp.
  struct DbgHelpLock
  {
    HANDLE handle = NULL;
    bool   held   = false;
    DbgHelpLock()
    {
      handle = EnsureDbgHelpMutex();
      if (handle == NULL) return;
      const DWORD r = WaitForSingleObject(handle, kDbgHelpMutexTimeoutMs);
      held = (r == WAIT_OBJECT_0) || (r == WAIT_ABANDONED);
    }
    ~DbgHelpLock()
    {
      if (held && handle != NULL) ReleaseMutex(handle);
    }
    bool acquired() const { return held; }
    DbgHelpLock(const DbgHelpLock&) = delete;
    DbgHelpLock& operator=(const DbgHelpLock&) = delete;
  };

  // Best-effort module-list refresh so SymFunctionTableAccess64 has the
  // current process map regardless of who called SymInitialize first.
  void RefreshSymModuleListLocked()
  {
    __try
    {
      SymRefreshModuleList(GetCurrentProcess());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      // DbgHelp internals can fault under heavy contention. Best-effort.
    }
  }

  void EnsureSymInit()
  {
    std::call_once(gSymInitOnce, []() {
      DbgHelpLock lock;
      if (!lock.acquired())
      {
        // Couldn't take the cross-DLL lock — leave gSymInitOk false; the
        // caller will degrade to a no-frames event.
        gSymInitOk.store(false, std::memory_order_release);
        return;
      }
      __try
      {
        // Options must be set BEFORE SymInitialize so the initial module
        // enumeration picks them up. Deferred loads keep startup cheap.
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        const BOOL ok = SymInitialize(GetCurrentProcess(), NULL, TRUE);
        if (ok)
        {
          // Belt-and-braces — DLLs loaded between SymInitialize and our
          // first walk should still be in the module table.
          RefreshSymModuleListLocked();
          gSymInitOk.store(true, std::memory_order_release);
          return;
        }
        // Already initialised by another module in the same process — fine.
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER)
        {
          // The prior caller may have used fInvadeProcess=FALSE which
          // leaves the module table empty. Force-populate it now.
          RefreshSymModuleListLocked();
          gSymInitOk.store(true, std::memory_order_release);
          return;
        }
        // Anything else (access denied, OOM, …) — symbol resolution will
        // degrade. We mark not-ok so the caller bails to no-frames rather
        // than ship potentially corrupt walks.
        gSymInitOk.store(false, std::memory_order_release);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        gSymInitOk.store(false, std::memory_order_release);
      }
    });
  }

  // Append a debug_meta image entry built from a single SymGetModuleInfo64
  // record. The debug-id format follows sentry-native's pe convention:
  //   "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX-AGE"
  // where the leading 32 hex chars come from PdbSig70 (GUID) and AGE is
  // the lowercase hex PdbAge. The Sentry symbolicator uses this together
  // with Stream D's uploaded .pdb to resolve raw RIPs server-side.
  void AppendDebugImage(sentry_value_t images, const IMAGEHLP_MODULE64& mod)
  {
    char debugId[64];
    std::snprintf(debugId, sizeof(debugId),
      "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x-%lx",
      (unsigned long) mod.PdbSig70.Data1,
      (unsigned)      mod.PdbSig70.Data2,
      (unsigned)      mod.PdbSig70.Data3,
      (unsigned)      mod.PdbSig70.Data4[0], (unsigned) mod.PdbSig70.Data4[1],
      (unsigned)      mod.PdbSig70.Data4[2], (unsigned) mod.PdbSig70.Data4[3],
      (unsigned)      mod.PdbSig70.Data4[4], (unsigned) mod.PdbSig70.Data4[5],
      (unsigned)      mod.PdbSig70.Data4[6], (unsigned) mod.PdbSig70.Data4[7],
      (unsigned long) mod.PdbAge);

    char imageAddr[32];
    std::snprintf(imageAddr, sizeof(imageAddr), "0x%llx",
                  (unsigned long long) mod.BaseOfImage);

    sentry_value_t image = sentry_value_new_object();
    sentry_value_set_by_key(image, "type", sentry_value_new_string("pe"));
    sentry_value_set_by_key(image, "image_addr",
                            sentry_value_new_string(imageAddr));
    sentry_value_set_by_key(image, "image_size",
                            sentry_value_new_int32((int32_t) mod.ImageSize));
    sentry_value_set_by_key(image, "code_file",
                            sentry_value_new_string(mod.ImageName));
    sentry_value_set_by_key(image, "debug_id",
                            sentry_value_new_string(debugId));
    if (mod.LoadedPdbName[0] != '\0')
    {
      sentry_value_set_by_key(image, "debug_file",
                              sentry_value_new_string(mod.LoadedPdbName));
    }
    sentry_value_append(images, image);
  }

  // Build a debug_meta block covering every module whose load range covers
  // at least one captured PC. Caller MUST hold the DbgHelp mutex.
  sentry_value_t BuildDebugMetaImages(const uint64_t* addrs, size_t addrCount)
  {
    if (addrCount == 0) return sentry_value_new_null();
    sentry_value_t images = sentry_value_new_list();
    size_t emitted = 0;

    // Dedupe by base address — the same module typically covers many PCs.
    uint64_t seenBases[IPLUG_WATCHDOG_MAX_DEBUG_IMAGES];
    size_t seenCount = 0;

    const HANDLE hProcess = GetCurrentProcess();
    for (size_t i = 0; i < addrCount && emitted < kMaxDebugImages; ++i)
    {
      const DWORD64 base = SymGetModuleBase64(hProcess, addrs[i]);
      if (base == 0) continue;

      bool already = false;
      for (size_t j = 0; j < seenCount; ++j)
        if (seenBases[j] == base) { already = true; break; }
      if (already) continue;
      if (seenCount < kMaxDebugImages) seenBases[seenCount++] = base;

      IMAGEHLP_MODULE64 modInfo;
      std::memset(&modInfo, 0, sizeof(modInfo));
      modInfo.SizeOfStruct = sizeof(modInfo);
      __try
      {
        if (!SymGetModuleInfo64(hProcess, base, &modInfo)) continue;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        continue;
      }
      // Need a usable PdbSig70 for the debug-id; if it's all zeroes, the
      // .pdb won't symbolicate this image and the entry is just noise.
      const GUID& g = modInfo.PdbSig70;
      const bool sigEmpty =
        g.Data1 == 0 && g.Data2 == 0 && g.Data3 == 0 &&
        g.Data4[0] == 0 && g.Data4[1] == 0 && g.Data4[2] == 0 &&
        g.Data4[3] == 0 && g.Data4[4] == 0 && g.Data4[5] == 0 &&
        g.Data4[6] == 0 && g.Data4[7] == 0;
      if (sigEmpty) continue;

      AppendDebugImage(images, modInfo);
      ++emitted;
    }

    if (emitted == 0)
    {
      sentry_value_decref(images);
      return sentry_value_new_null();
    }
    return images;
  }

  // Build the tags/extras the no-frames stub used to emit, plus optional
  // thread + stacktrace + debug_meta. Mirrors the macOS event shape so
  // Sentry's issue grouper coalesces Windows and macOS hangs of the same
  // root cause.
  //
  //   - sentry_value_new_event() (NOT message_event) — keeps shape symmetric
  //     with macOS and avoids the message_event fingerprint divergence.
  //   - threads attached as a raw list (no `{values:[...]}` wrapper) to
  //     match macOS exactly.
  //   - thread.id carries the wedged audio thread's tid.
  //   - extras carry stall_duration_ms, block_size, sample_rate (macOS parity).
  //   - debug_meta.images is attached when present so the server symbolicator
  //     can match raw RIPs against Stream D's uploaded .pdbs by debug-id.
  sentry_value_t BuildHangEvent(uint32_t audioTid,
                                std::chrono::milliseconds stallMs,
                                int32_t blockSize, double sampleRate,
                                sentry_value_t framesOrNull /* may be null */,
                                sentry_value_t debugImagesOrNull /* may be null */,
                                bool truncatedAtMax,
                                DWORD resumeResult)
  {
    sentry_value_t event = sentry_value_new_event();
    sentry_value_set_by_key(event, "level", sentry_value_new_string("warning"));
    sentry_value_set_by_key(event, "logger",
      sentry_value_new_string("iplug.watchdog"));
    sentry_value_set_by_key(event, "message",
      sentry_value_new_string("iPlug2 watchdog detected stalled ProcessBlock"));

    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "event_type",
      sentry_value_new_string("audio_thread_hang"));
    char durBuf[32];
    std::snprintf(durBuf, sizeof(durBuf), "%lld", (long long) stallMs.count());
    sentry_value_set_by_key(tags, "stall_duration_ms",
      sentry_value_new_string(durBuf));
    if (truncatedAtMax)
    {
      sentry_value_set_by_key(tags, "truncated_at_max",
        sentry_value_new_string("true"));
    }
    if (resumeResult == (DWORD) -1)
    {
      // ResumeThread failed entirely — surface so we can spot stuck-audio
      // incidents server-side. Operator action would be: kill the host
      // process; we cannot recover from outside.
      sentry_value_set_by_key(tags, "resume_thread_failed",
        sentry_value_new_string("true"));
    }
    else if (resumeResult > 1)
    {
      // Prior suspend count > 1 — something else suspended the audio
      // thread independently. We dropped it by one, it remains suspended.
      char psBuf[16];
      std::snprintf(psBuf, sizeof(psBuf), "%lu", (unsigned long) resumeResult);
      sentry_value_set_by_key(tags, "prior_suspend_count",
        sentry_value_new_string(psBuf));
    }
    sentry_value_set_by_key(event, "tags", tags);

    // Numeric extras — Discover numeric filters (stall_duration_ms:>500)
    // and Windows-vs-macOS dashboards depend on these, mirror macOS path.
    sentry_value_t extra = sentry_value_new_object();
    sentry_value_set_by_key(extra, "stall_duration_ms",
      sentry_value_new_int32((int32_t) stallMs.count()));
    sentry_value_set_by_key(extra, "block_size",
      sentry_value_new_int32(blockSize));
    sentry_value_set_by_key(extra, "sample_rate",
      sentry_value_new_double(sampleRate));
    sentry_value_set_by_key(event, "extra", extra);

    if (!sentry_value_is_null(framesOrNull))
    {
      sentry_value_t stacktrace = sentry_value_new_object();
      sentry_value_set_by_key(stacktrace, "frames", framesOrNull);

      sentry_value_t thread = sentry_value_new_object();
      sentry_value_set_by_key(thread, "id",
        sentry_value_new_int32((int32_t) audioTid));
      sentry_value_set_by_key(thread, "name",
        sentry_value_new_string("audio"));
      sentry_value_set_by_key(thread, "crashed", sentry_value_new_bool(0));
      sentry_value_set_by_key(thread, "stacktrace", stacktrace);

      // Raw list shape — matches macOS so the grouper converges.
      sentry_value_t threads = sentry_value_new_list();
      sentry_value_append(threads, thread);
      sentry_value_set_by_key(event, "threads", threads);
    }

    if (!sentry_value_is_null(debugImagesOrNull))
    {
      sentry_value_t debugMeta = sentry_value_new_object();
      sentry_value_set_by_key(debugMeta, "images", debugImagesOrNull);
      sentry_value_set_by_key(event, "debug_meta", debugMeta);
    }

    return event;
  }

  void EmitNoFramesHangEvent(uint32_t audioTid,
                             std::chrono::milliseconds stallMs,
                             int32_t blockSize, double sampleRate,
                             DWORD resumeResult = 0)
  {
    if (gShuttingDown.load(std::memory_order_relaxed)) return;
    sentry_value_t event = BuildHangEvent(audioTid, stallMs, blockSize,
                                          sampleRate,
                                          sentry_value_new_null(),
                                          sentry_value_new_null(),
                                          /*truncatedAtMax=*/false,
                                          resumeResult);
    sentry_capture_event(event);
  }

  // Real cross-thread stack walker. Mirrors the macOS unsandboxed pattern:
  //   1. Identify the audio thread (slot.audioWinThreadId, captured on the
  //      first Tick).
  //   2. Open a limited-rights handle (no THREAD_QUERY_INFORMATION — see
  //      review note; StackWalk64's ReadProcessMemory uses the process
  //      handle, not the thread handle).
  //   3. SuspendThread → GetThreadContext → ResumeThread (window minimised
  //      — no allocation, no sentry calls between suspend and resume).
  //   4. Verify the suspended thread is still our audio thread (TID-reuse
  //      defense via GetExitCodeThread + GetThreadTimes.CreationTime).
  //   5. StackWalk64 against the captured CONTEXT under the cross-DLL
  //      DbgHelp mutex.
  //   6. Build + ship the sentry event AFTER resume, gated on shutdown.
  void CaptureStall_Windows(HeartbeatSlot& slot,
                            uint16_t expectedGen,
                            std::chrono::milliseconds stallMs,
                            int32_t blockSize, double sampleRate)
  {
    const uint32_t tid = slot.audioWinThreadId.load(std::memory_order_acquire);
    if (tid == 0)
    {
      // First Tick hasn't run yet — no audio thread identity to walk. Fall
      // back to the original no-frames event so dashboards still alert.
      EmitNoFramesHangEvent(0, stallMs, blockSize, sampleRate);
      return;
    }

    // Recheck the generation now that we're inside CaptureStall — Unregister
    // may have run since CheckSlot last sampled. Belt-and-braces vs the
    // tid-reuse race window described in the review.
    if (slot.generation.load(std::memory_order_acquire) != expectedGen)
    {
      EmitNoFramesHangEvent(0, stallMs, blockSize, sampleRate);
      return;
    }

  #if !defined(_M_X64) && !defined(_M_AMD64)
    // ARM64 / ARM64EC StackWalk64 requires different CONTEXT register slots
    // (Pc/Sp/Fp) and IMAGE_FILE_MACHINE_ARM64 — out of scope for this PR.
    // The fork's Windows ARM64EC build skips by design (per cmake-ci.yml).
    (void) tid;
    EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate);
    return;
  #else
    // Defensive: never suspend ourselves. Compare against the watchdog's
    // OWN tid captured once at ThreadMain entry (not GetCurrentThreadId
    // recomputed here — more robust against any future APC-driven
    // identity weirdness).
    const uint32_t selfTid = gWatchdogTid.load(std::memory_order_relaxed);
    if (selfTid != 0 && tid == selfTid)
    {
      EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate);
      return;
    }

    // No THREAD_QUERY_INFORMATION: StackWalk64's ReadProcessMemory uses the
    // process handle, not the thread handle, so we don't need it. Some
    // tightened-token DAWs deny it; dropping it widens compatibility.
    // THREAD_QUERY_LIMITED_INFORMATION is added so we can call
    // GetExitCodeThread / GetThreadTimes for the tid-reuse defense.
    HANDLE hThread = OpenThread(
      THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
        THREAD_QUERY_LIMITED_INFORMATION,
      FALSE, (DWORD) tid);
    if (hThread == NULL)
    {
      // Could be ERROR_ACCESS_DENIED if the audio thread already died, or
      // the host runs with sandboxing that strips THREAD_* rights. Either
      // way, ship the no-frames variant.
      EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate);
      return;
    }

    // TID-reuse defense BEFORE suspending. If the kernel thread we
    // captured at first-Tick is already dead, the tid has either been
    // recycled (we'd OpenThread a different thread) or the handle is
    // valid but the thread is gone — GetExitCodeThread tells us.
    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeThread(hThread, &exitCode) && exitCode != STILL_ACTIVE)
    {
      CloseHandle(hThread);
      EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate);
      return;
    }

    // Second-line TID-reuse defense: compare CreationTime snapshot against
    // what we cached on the first Tick. CreationTime is a kernel-side
    // stable identity that survives recycle — if it mismatches, the tid
    // is now owned by a different thread (an unrelated UI worker, host
    // GC, sibling plugin's audio thread, ...) and we MUST NOT suspend.
    const uint32_t expectedLow  =
      slot.audioWinCreationTimeLow.load(std::memory_order_relaxed);
    const uint32_t expectedHigh =
      slot.audioWinCreationTimeHigh.load(std::memory_order_relaxed);
    if (expectedLow != 0 || expectedHigh != 0)
    {
      FILETIME ftCreate, ftExit, ftKernel, ftUser;
      if (GetThreadTimes(hThread, &ftCreate, &ftExit, &ftKernel, &ftUser))
      {
        if (ftCreate.dwLowDateTime != expectedLow ||
            ftCreate.dwHighDateTime != expectedHigh)
        {
          CloseHandle(hThread);
          EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate);
          return;
        }
      }
    }

    // ----- Begin minimum-suspension window. NO allocations, NO sentry. -----
    const DWORD suspendResult = SuspendThread(hThread);
    if (suspendResult == (DWORD) -1)
    {
      CloseHandle(hThread);
      EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate);
      return;
    }

    CONTEXT context;
    std::memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;
    const BOOL gotContext = GetThreadContext(hThread, &context);

    // Resume IMMEDIATELY — before any allocation, before any sentry_value
    // call. We may not have a context, but we are NOT going to do any work
    // while the audio thread is suspended. Capture the return so a silent
    // failure becomes a tagged event rather than a permanent suspend.
    DWORD resumeResult = ResumeThread(hThread);
    if (resumeResult == (DWORD) -1)
    {
      // Retry — these are non-allocating syscalls and we're already
      // outside the critical window. If every retry fails the audio
      // thread is stuck; we still ship the event with resume_thread_failed
      // tagged so triage at least surfaces the case.
      for (int i = 0; i < kResumeRetryCount; ++i)
      {
        const DWORD r = ResumeThread(hThread);
        if (r != (DWORD) -1) { resumeResult = r; break; }
      }
    }
    // ----- End minimum-suspension window. -----

    // Re-check shutdown right after the window — if atexit started while
    // we were suspended, sentry-native's transport may be in teardown.
    if (gShuttingDown.load(std::memory_order_relaxed))
    {
      CloseHandle(hThread);
      return;
    }

    if (!gotContext)
    {
      CloseHandle(hThread);
      EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate, resumeResult);
      return;
    }

    // Symbol init. EnsureSymInit was already called eagerly from ThreadMain;
    // this is the fallback for the unlikely case we got here without it.
    EnsureSymInit();
    if (!gSymInitOk.load(std::memory_order_acquire))
    {
      CloseHandle(hThread);
      EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate, resumeResult);
      return;
    }

    // STACKFRAME64 init: on x64, StackWalk64 relies on SymFunctionTableAccess64
    // (i.e. .pdata-based unwind tables) and AddrFrame is included only
    // because the API requires it to be initialised — Rbp may or may not be
    // the actual frame pointer depending on FPO (`/Oy`).
    STACKFRAME64 frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset    = context.Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;

    const HANDLE hProcess = GetCurrentProcess();

    // Sentry expects frames in oldest-first order (caller-before-callee).
    // StackWalk64 hands them to us in newest-first order (innermost first),
    // so we collect addresses then push to the sentry list in reverse.
    uint64_t addrs[kMaxStackFrames];
    size_t addrCount = 0;
    bool   truncatedAtMax = false;

    // Stack walk + debug image enumeration both touch DbgHelp internals —
    // serialise them under the cross-DLL mutex so a sibling iPlug2 plugin
    // in the same host can't corrupt DbgHelp's per-process state.
    sentry_value_t debugImages = sentry_value_new_null();
    {
      DbgHelpLock dlock;
      if (!dlock.acquired())
      {
        // Couldn't take the lock within the timeout — ship a no-frames
        // event rather than risk a corrupting concurrent walk.
        CloseHandle(hThread);
        EmitNoFramesHangEvent(tid, stallMs, blockSize, sampleRate, resumeResult);
        return;
      }

      // Wrap DbgHelp interactions in SEH so a fault inside StackWalk64
      // (DbgHelp races with host crash reporters etc.) doesn't take down
      // the host. Converts a 'DbgHelp crashed' incident into 'no frames'.
      uint64_t prevRsp = 0;
      uint64_t prevPc  = 0;
      __try
      {
        while (addrCount < kMaxStackFrames)
        {
          const BOOL ok = StackWalk64(
            IMAGE_FILE_MACHINE_AMD64,
            hProcess,
            hThread,
            &frame,
            &context,
            NULL,                            // ReadMemoryRoutine — default
            SymFunctionTableAccess64,
            SymGetModuleBase64,
            NULL);                           // TranslateAddress — default
          if (!ok) break;
          const uint64_t pc  = frame.AddrPC.Offset;
          const uint64_t rsp = frame.AddrStack.Offset;
          if (pc == 0) break;
          // Detect a stuck unwinder: x64 stack grows down so a successor
          // frame's Rsp must be strictly greater than its callee's. Also
          // catch PC duplication (same PC twice means StackWalk64 is
          // looping). Both indicate corrupted unwind info — bail rather
          // than ship 64 frames of garbage.
          if (addrCount > 0 && (rsp <= prevRsp || pc == prevPc)) break;
          addrs[addrCount++] = pc;
          prevRsp = rsp;
          prevPc  = pc;
        }
        if (addrCount == kMaxStackFrames) truncatedAtMax = true;

        // Build debug_meta covering modules touched by the captured PCs.
        // Must happen under the same DbgHelp lock since SymGetModuleBase64
        // / SymGetModuleInfo64 are not thread-safe.
        debugImages = BuildDebugMetaImages(addrs, addrCount);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        addrCount      = 0;
        truncatedAtMax = false;
        // debugImages stays sentry_value_new_null() from the initialiser.
      }
    } // DbgHelp mutex released

    // Done with the audio-thread handle. Symbolication is server-side.
    CloseHandle(hThread);

    // One more shutdown check before allocating the event — by now we may
    // have spent real time inside StackWalk64.
    if (gShuttingDown.load(std::memory_order_relaxed))
    {
      if (!sentry_value_is_null(debugImages)) sentry_value_decref(debugImages);
      return;
    }

    sentry_value_t framesArray = sentry_value_new_null();
    if (addrCount > 0)
    {
      framesArray = sentry_value_new_list();
      // Reverse: Sentry's frames list is bottom-up (oldest first).
      for (size_t i = addrCount; i > 0; --i)
      {
        const uint64_t pc = addrs[i - 1];
        char hexBuf[2 + 16 + 1]; // "0x" + 16 hex digits + NUL
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%llx",
                      (unsigned long long) pc);
        sentry_value_t f = sentry_value_new_object();
        sentry_value_set_by_key(f, "instruction_addr",
          sentry_value_new_string(hexBuf));
        sentry_value_append(framesArray, f);
      }
    }

    sentry_value_t event = BuildHangEvent(tid, stallMs, blockSize, sampleRate,
                                          framesArray, debugImages,
                                          truncatedAtMax, resumeResult);
    sentry_capture_event(event);
  #endif // x64 gate
  }
#endif // _WIN32

  void CaptureStall(HeartbeatSlot& slot,
                    uint16_t expectedGen,
                    std::chrono::milliseconds stallMs)
  {
    // Hard cap per session to avoid flooding Sentry on a thrashing host.
    // Counter is monotonic-on-attempt (a TOCTOU-cleaner load+fetch_add
    // pair is documented in the reviews but not adopted — the budget is
    // approximate-by-design and the count is bounded by per-iteration
    // tick rate, not by adversarial input).
    if (gHangEventCount.fetch_add(1, std::memory_order_relaxed)
          >= kMaxHangEventsPerSession)
      return;
    // Bail if shutdown started — sentry-native transport may be torn down.
    if (gShuttingDown.load(std::memory_order_relaxed)) return;

    const int32_t bs = slot.blockSize.load(std::memory_order_relaxed);
    const double  sr = slot.sampleRate.load(std::memory_order_relaxed);

#if defined(_WIN32)
    CaptureStall_Windows(slot, expectedGen, stallMs, bs, sr);
#elif defined(__APPLE__)
    (void) expectedGen;
    const uint32_t machPort = slot.audioMachThread.load(std::memory_order_relaxed);
    CaptureStall_macOS(machPort, stallMs, bs, sr);
#else
    (void) slot;
    (void) expectedGen;
    (void) stallMs;
    (void) bs;
    (void) sr;
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
    // Pass genAfter as the expected generation — CaptureStall_Windows will
    // re-check after Unregister's potential racing bump.
    CaptureStall(slot, genAfter, elapsed);
  }

  void ThreadMain()
  {
#if defined(_WIN32)
    // Capture our own tid once so the self-suspend guard is unambiguous,
    // and warm SymInitialize eagerly so its tens-to-hundreds-of-ms first-
    // call cost lands at plugin-load time rather than inside the first
    // real stall (where it would back-pressure subsequent CheckSlot
    // iterations and create a lock-order risk vs host crash handlers).
    gWatchdogTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    EnsureSymInit();
#endif
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
#if defined(_WIN32)
    slot.audioWinThreadId.store(0, std::memory_order_relaxed);
    slot.audioWinCreationTimeLow.store(0, std::memory_order_relaxed);
    slot.audioWinCreationTimeHigh.store(0, std::memory_order_relaxed);
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
#if defined(_WIN32)
  // We only ever stored a raw thread id (no HANDLE), so there is nothing
  // to CloseHandle here. Clearing both the tid and the CreationTime
  // snapshot lets a future Tick on a reclaimed slot re-capture the new
  // audio thread's identity cleanly. Generation bump below carries the
  // visibility for these stores too.
  slot.audioWinThreadId.store(0, std::memory_order_relaxed);
  slot.audioWinCreationTimeLow.store(0, std::memory_order_relaxed);
  slot.audioWinCreationTimeHigh.store(0, std::memory_order_relaxed);
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

#if defined(_WIN32)
  // Record the audio thread's Win32 tid the first time. GetCurrentThreadId
  // is a TEB read (FS:[0x48] on x64) — ~1-2 cycles, no syscall, RT-safe.
  // We deliberately do NOT call GetCurrentThread() (returns a pseudo-handle
  // that's useless from another thread) or OpenThread() (syscall — must
  // never run on the audio thread). The watchdog opens a fresh limited-
  // rights handle on demand when it needs to walk the stack.
  //
  // We also snapshot the kernel-side CreationTime via GetThreadTimes (a
  // cheap syscall, but only on the COLD first-Tick path — once per slot
  // lifetime) so the watchdog can detect TID reuse: Windows recycles
  // thread IDs aggressively once a thread exits, and CreationTime is the
  // stable per-thread identity that survives recycling.
  //
  // Ordering: tid stored with release so the watchdog's acquire load
  // serialises observability of the CreationTime halves too. Without the
  // release we'd be relying on x86 TSO and the code would be UB on a
  // future ARM Windows port.
  const uint32_t prevWinTid = slot.audioWinThreadId.load(std::memory_order_relaxed);
  if (prevWinTid == 0)
  {
    // GetThreadTimes against the current-thread pseudo-handle is RT-safe
    // enough for the cold first-Tick path (one syscall, no allocations).
    FILETIME ftCreate, ftExit, ftKernel, ftUser;
    if (GetThreadTimes(GetCurrentThread(), &ftCreate, &ftExit, &ftKernel, &ftUser))
    {
      slot.audioWinCreationTimeLow.store(ftCreate.dwLowDateTime,
                                         std::memory_order_relaxed);
      slot.audioWinCreationTimeHigh.store(ftCreate.dwHighDateTime,
                                          std::memory_order_relaxed);
    }
    const uint32_t tid = static_cast<uint32_t>(GetCurrentThreadId());
    slot.audioWinThreadId.store(tid, std::memory_order_release);
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
