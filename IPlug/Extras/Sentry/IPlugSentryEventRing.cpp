/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#include "IPlugSentryEventRing.h"

#ifdef IPLUG_USE_SENTRY

  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <future>
  #include <mutex>
  #include <string>
  #include <thread>

  #if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shlobj.h>
  #elif defined(__APPLE__)
    #include <mach/mach_time.h>
    #include <sys/stat.h>
    #include <unistd.h>
  #endif

namespace iplug
{
namespace sentry
{
namespace eventring
{

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "event ring requires lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "event ring requires lock-free 32-bit atomics");
static_assert(std::atomic<bool>::is_always_lock_free,
              "event ring requires lock-free atomic<bool>");

// Public — published from this TU so the probe seam + the audio detectors
// share the same clock source. Without this they sort wrong in the rendered
// audio_events.log (different monotonic epochs).
uint64_t NowNs() noexcept
{
#if defined(__APPLE__)
  static mach_timebase_info_data_t tb = {0, 0};
  if (tb.denom == 0) mach_timebase_info(&tb);
  const uint64_t t = mach_absolute_time();
  return (t * tb.numer) / tb.denom;
#elif defined(_WIN32)
  static LARGE_INTEGER freq = {};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const uint64_t qpc = (uint64_t) now.QuadPart;
  const uint64_t f   = (uint64_t) freq.QuadPart;
  const uint64_t whole = qpc / f;
  const uint64_t frac  = qpc % f;
  return whole * 1000000000ull + (frac * 1000000000ull) / f;
#else
  return 0;
#endif
}

namespace
{
  // Ring sizing. 4096 slots * 32 bytes/slot = 128 KiB per DLL. Power of
  // two so the modulo collapses to a bitwise AND.
  constexpr uint32_t kRingCapacity = 4096;
  constexpr uint32_t kRingMask     = kRingCapacity - 1;
  static_assert((kRingCapacity & kRingMask) == 0,
                "ring capacity must be a power of two");

  // Rendered text ring. 256 KiB — enough for several minutes of audio
  // events at typical rates, capped so it cannot bloat memory unbounded.
  constexpr size_t kRenderedCapacity = 256 * 1024;

  // Attachment ceiling — we cap the file on disk so a long-running session
  // doesn't ship a giant attachment per Sentry event.
  constexpr size_t kAttachmentMaxBytes = 64 * 1024;

  // Stage name table sizing.
  constexpr size_t kStageNameTableCap   = 64;
  constexpr size_t kStageNameMaxLen     = 63;  // + 1 for NUL

  // Drain wakeup cadence. Short enough that the rendered buffer + the
  // on-disk attachment stay current to within ~50ms of any audio event;
  // long enough that the drain thread spends most of its time blocked.
  constexpr std::chrono::milliseconds kDrainWakeInterval{50};

  // How often we rewrite the on-disk attachment file. Once a second is
  // enough for hang/crash forensics — the rendered text ring is the
  // authoritative source; the file is just a snapshot for sentry-native.
  constexpr std::chrono::milliseconds kAttachmentFlushInterval{1000};

  // Bounded join on stop — mirror the watchdog's pattern.
  constexpr std::chrono::milliseconds kStopJoinTimeout{200};

  // SPSC ring. Single producer / single consumer. Producer = audio
  // thread; consumer = our dedicated drain thread. Push: load reader
  // (acquire), compute used = writer - reader; if full, increment
  // overflow counter and bail. Else memcpy the record into the slot,
  // then store writer with release. Pop is symmetric.
  alignas(64) EventRecord gSlots[kRingCapacity];
  alignas(64) std::atomic<uint64_t> gWriterIdx{0};
  alignas(64) std::atomic<uint64_t> gReaderIdx{0};
  alignas(64) std::atomic<uint64_t> gOverflowCount{0};

  // Stage-name intern table. Audio-thread-safe lookup: linear scan of
  // up to 64 short C strings, each fitting in a single cache line. On
  // a miss the cold-path tries to take a try-lock and append; failure
  // (table full OR contested lock) returns id=0 ("unknown") rather
  // than waiting.
  struct StageNameEntry
  {
    char name[kStageNameMaxLen + 1] = {0};
    uint32_t len = 0; // 0 == slot unused
  };

  StageNameEntry gStageNames[kStageNameTableCap];
  std::atomic<uint32_t> gStageNameCount{0};
  std::atomic_flag gStageAppendLock = ATOMIC_FLAG_INIT;

  uint32_t StrLenBounded(const char* s) noexcept
  {
    if (!s) return 0;
    uint32_t n = 0;
    while (n < kStageNameMaxLen && s[n] != '\0') ++n;
    return n;
  }

  bool StringsEqualBounded(const char* a, uint32_t aLen,
                           const char* b, uint32_t bLen) noexcept
  {
    if (aLen != bLen) return false;
    for (uint32_t i = 0; i < aLen; ++i)
      if (a[i] != b[i]) return false;
    return true;
  }

  // Rendered text buffer. Drain thread writes; capture readers read
  // under the same mutex. Simple circular byte buffer — when full,
  // oldest bytes get overwritten by the writer's wrap-around.
  struct RenderedBuffer
  {
    std::mutex mu;
    char  data[kRenderedCapacity];
    size_t totalWritten = 0; // monotonic; modulo capacity = write offset
  };

  RenderedBuffer gRendered;

  void RenderedAppendLocked(const char* src, size_t len) noexcept
  {
    if (len == 0) return;
    if (len > kRenderedCapacity)
    {
      src += (len - kRenderedCapacity);
      len = kRenderedCapacity;
    }
    const size_t writeOffset = gRendered.totalWritten & (kRenderedCapacity - 1);
    static_assert((kRenderedCapacity & (kRenderedCapacity - 1)) == 0,
                  "rendered capacity must be a power of two for mask offset");
    const size_t firstChunk =
      (writeOffset + len <= kRenderedCapacity) ? len : (kRenderedCapacity - writeOffset);
    std::memcpy(gRendered.data + writeOffset, src, firstChunk);
    if (firstChunk < len)
      std::memcpy(gRendered.data, src + firstChunk, len - firstChunk);
    gRendered.totalWritten += len;
  }

  // Drain thread state.
  std::thread gDrainThread;
  std::once_flag gDrainStartOnce;
  std::atomic<bool> gDrainStopRequested{false};
  std::atomic<bool> gDrainAlive{false};
  std::mutex gDrainWakeMutex;
  std::condition_variable gDrainWakeCv;

  std::string gAttachmentPath;

  const char* KindLabel(uint16_t kind) noexcept
  {
    switch (kind)
    {
      case EVENT_KIND_BLOCK_CPU_OVER_80: return "BLOCK_CPU_OVER_80";
      case EVENT_KIND_NAN_OUTPUT:        return "NAN_OUTPUT";
      case EVENT_KIND_INF_OUTPUT:        return "INF_OUTPUT";
      case EVENT_KIND_PROBE_NAN:         return "PROBE_NAN";
      case EVENT_KIND_PROBE_SILENCE:     return "PROBE_SILENCE";
      case EVENT_KIND_PROBE_CPU_OVER:    return "PROBE_CPU_OVER";
      case EVENT_KIND_RING_OVERFLOW:     return "RING_OVERFLOW";
      default:                           return "UNKNOWN";
    }
  }

  const char* StageIdToName(uint32_t stageId) noexcept
  {
    if (stageId == 0) return "";
    if (stageId > kStageNameTableCap) return "";
    const StageNameEntry& e = gStageNames[stageId - 1];
    if (e.len == 0) return "";
    return e.name;
  }

  size_t RenderEvent(const EventRecord& rec, char* out, size_t outCap) noexcept
  {
    if (outCap == 0) return 0;
    const unsigned long long tsMs = (unsigned long long)(rec.timestampNs / 1000000ull);
    const char* label = KindLabel(rec.kind);
    const char* stage = StageIdToName(rec.stageId);
    int n;
    if (stage[0] != '\0')
    {
      n = std::snprintf(out, outCap,
        "[%llu] %s stage=%s ch=%d value=%.4f\n",
        tsMs, label, stage, (int) rec.channel, (double) rec.value);
    }
    else
    {
      n = std::snprintf(out, outCap,
        "[%llu] %s ch=%d value=%.4f\n",
        tsMs, label, (int) rec.channel, (double) rec.value);
    }
    if (n < 0) return 0;
    if ((size_t) n >= outCap) return outCap - 1;
    return (size_t) n;
  }

  void RenderOverflowNotice(uint64_t lostSinceLast, char* out, size_t outCap) noexcept
  {
    std::snprintf(out, outCap,
      "[--] RING_OVERFLOW lost=%llu\n",
      (unsigned long long) lostSinceLast);
  }

  // Attachment path resolution. Mirrors the database-path logic in
  // IPlugSentry.cpp but keeps a SINGLE file at a stable, plugin-id-aware
  // location so the init-time sentry_options_add_attachment call can
  // point at it. The drain thread refreshes the file on a timer.
  std::string ResolveAttachmentPath()
  {
  #if defined(_WIN32)
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
      return "";
    char buf[MAX_PATH] = {0};
    WideCharToMultiByte(CP_UTF8, 0, localAppData, -1, buf, MAX_PATH, nullptr, nullptr);
    CoTaskMemFree(localAppData);
    std::string path(buf);
    path += "\\PluginMaker\\sentry-db\\";
  #ifdef PLUGIN_ID
    path += (PLUGIN_ID[0] != '\0') ? PLUGIN_ID : "unknown";
  #else
    path += "unknown";
  #endif
    CreateDirectoryA(path.c_str(), nullptr);
    path += "\\audio_events.log";
    return path;
  #elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home || !*home) return "";
    std::string path(home);
    path += "/Library/Application Support/PluginMaker/sentry-db/";
  #ifdef PLUGIN_ID
    path += (PLUGIN_ID[0] != '\0') ? PLUGIN_ID : "unknown";
  #else
    path += "unknown";
  #endif
    {
      std::string dir = path;
      size_t slash = 0;
      while ((slash = dir.find('/', slash + 1)) != std::string::npos)
      {
        std::string parent = dir.substr(0, slash);
        ::mkdir(parent.c_str(), 0755);
      }
      ::mkdir(dir.c_str(), 0755);
    }
    path += "/audio_events.log";
    return path;
  #else
    return "";
  #endif
  }

  void FlushAttachmentToDisk() noexcept
  {
    if (gAttachmentPath.empty()) return;
    char snapshot[kAttachmentMaxBytes];
    const size_t n = CopyRenderedTail(snapshot, sizeof(snapshot));
    if (n == 0) return;
    std::string tmp = gAttachmentPath + ".tmp";
    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) return;
    const size_t written = std::fwrite(snapshot, 1, n, fp);
    std::fclose(fp);
    if (written != n) { std::remove(tmp.c_str()); return; }
  #if defined(_WIN32)
    MoveFileExA(tmp.c_str(), gAttachmentPath.c_str(),
                MOVEFILE_REPLACE_EXISTING);
  #else
    std::rename(tmp.c_str(), gAttachmentPath.c_str());
  #endif
  }

  void DrainThreadMain()
  {
    using clock = std::chrono::steady_clock;
    auto lastFlush = clock::now();
    uint64_t lastSeenOverflow = 0;

    while (!gDrainStopRequested.load(std::memory_order_relaxed))
    {
      try
      {
        {
          std::unique_lock<std::mutex> lock(gDrainWakeMutex);
          gDrainWakeCv.wait_for(lock, kDrainWakeInterval, []{
            return gDrainStopRequested.load(std::memory_order_relaxed);
          });
        }
        if (gDrainStopRequested.load(std::memory_order_relaxed)) break;

        char scratch[256];
        for (;;)
        {
          const uint64_t r = gReaderIdx.load(std::memory_order_relaxed);
          const uint64_t w = gWriterIdx.load(std::memory_order_acquire);
          if (r == w) break;
          EventRecord rec;
          std::memcpy(&rec, &gSlots[r & kRingMask], sizeof(rec));
          gReaderIdx.store(r + 1, std::memory_order_release);

          const size_t n = RenderEvent(rec, scratch, sizeof(scratch));
          if (n > 0)
          {
            std::lock_guard<std::mutex> lk(gRendered.mu);
            RenderedAppendLocked(scratch, n);
          }
        }

        const uint64_t curOverflow =
          gOverflowCount.load(std::memory_order_relaxed);
        if (curOverflow > lastSeenOverflow)
        {
          const uint64_t lost = curOverflow - lastSeenOverflow;
          lastSeenOverflow = curOverflow;
          char overflowScratch[64];
          RenderOverflowNotice(lost, overflowScratch, sizeof(overflowScratch));
          {
            std::lock_guard<std::mutex> lk(gRendered.mu);
            RenderedAppendLocked(overflowScratch, std::strlen(overflowScratch));
          }
        }

        const auto now = clock::now();
        if (now - lastFlush >= kAttachmentFlushInterval)
        {
          FlushAttachmentToDisk();
          lastFlush = now;
        }
      }
      catch (...) {}
    }

    try { FlushAttachmentToDisk(); } catch (...) {}
    gDrainAlive.store(false, std::memory_order_release);
  }

  void AtExitStopDrain() { StopDrainThread(); }
} // anonymous namespace

bool Push(const EventRecord& rec) noexcept
{
  const uint64_t w = gWriterIdx.load(std::memory_order_relaxed);
  const uint64_t r = gReaderIdx.load(std::memory_order_acquire);
  const uint64_t used = w - r;
  if (used >= kRingCapacity)
  {
    gOverflowCount.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  std::memcpy(&gSlots[w & kRingMask], &rec, sizeof(rec));
  gWriterIdx.store(w + 1, std::memory_order_release);
  return true;
}

uint32_t InternStageName(const char* name) noexcept
{
  if (!name) return 0;
  const uint32_t inLen = StrLenBounded(name);
  if (inLen == 0) return 0;

  const uint32_t count = gStageNameCount.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < count; ++i)
  {
    const StageNameEntry& e = gStageNames[i];
    if (StringsEqualBounded(e.name, e.len, name, inLen))
      return i + 1;
  }

  if (gStageAppendLock.test_and_set(std::memory_order_acquire))
    return 0;

  const uint32_t countLocked = gStageNameCount.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < countLocked; ++i)
  {
    const StageNameEntry& e = gStageNames[i];
    if (StringsEqualBounded(e.name, e.len, name, inLen))
    {
      gStageAppendLock.clear(std::memory_order_release);
      return i + 1;
    }
  }

  uint32_t id = 0;
  if (countLocked < kStageNameTableCap)
  {
    StageNameEntry& e = gStageNames[countLocked];
    std::memcpy(e.name, name, inLen);
    e.name[inLen] = '\0';
    e.len = inLen;
    gStageNameCount.store(countLocked + 1, std::memory_order_release);
    id = countLocked + 1;
  }

  gStageAppendLock.clear(std::memory_order_release);
  return id;
}

void StartDrainThread()
{
  std::call_once(gDrainStartOnce, []() {
    if (const char* off = std::getenv("PLUGINMAKER_EVENT_RING_DISABLE"))
    {
      if (*off && *off != '0') return;
    }

    gDrainStopRequested.store(false, std::memory_order_relaxed);

    gAttachmentPath = ResolveAttachmentPath();

    try
    {
      gDrainThread = std::thread(DrainThreadMain);
    }
    catch (...)
    {
      gDrainAlive.store(false, std::memory_order_release);
      return;
    }

    // Publish with RELEASE last so audio-thread IsDrainRunning() loads with
    // acquire and observes both the running thread + the resolved
    // attachment path before any Push records start landing.
    gDrainAlive.store(true, std::memory_order_release);
    std::atexit(&AtExitStopDrain);
  });
}

bool IsDrainRunning() noexcept
{
  // Audio-thread fast-path gate. Acquire pairs with the release-store at
  // the end of StartDrainThread so a true result implies all prior init
  // (attachment path resolution, thread spawn) is visible.
  return gDrainAlive.load(std::memory_order_acquire);
}

void StopDrainThread()
{
  gDrainStopRequested.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(gDrainWakeMutex);
  }
  gDrainWakeCv.notify_all();

  if (!gDrainThread.joinable()) return;

  auto fut = std::async(std::launch::async, []() {
    if (gDrainThread.joinable()) gDrainThread.join();
  });
  if (fut.wait_for(kStopJoinTimeout) != std::future_status::ready)
  {
    if (gDrainThread.joinable()) gDrainThread.detach();
  }
}

std::size_t CopyRenderedTail(char* outBuf, std::size_t outBufCapacity) noexcept
{
  if (!outBuf || outBufCapacity == 0) return 0;
  std::lock_guard<std::mutex> lk(gRendered.mu);
  const size_t total = gRendered.totalWritten;
  const size_t available = (total < kRenderedCapacity) ? total : kRenderedCapacity;
  if (available == 0) return 0;

  const size_t want = (available < outBufCapacity) ? available : outBufCapacity;
  const size_t absStart = total - want;
  const size_t startOff = absStart & (kRenderedCapacity - 1);

  if (startOff + want <= kRenderedCapacity)
  {
    std::memcpy(outBuf, gRendered.data + startOff, want);
  }
  else
  {
    const size_t firstChunk = kRenderedCapacity - startOff;
    std::memcpy(outBuf, gRendered.data + startOff, firstChunk);
    std::memcpy(outBuf + firstChunk, gRendered.data, want - firstChunk);
  }
  return want;
}

} // namespace eventring
} // namespace sentry
} // namespace iplug

#else // !IPLUG_USE_SENTRY

// OFF build: header provides inline no-op definitions. This TU is empty.

#endif // IPLUG_USE_SENTRY
