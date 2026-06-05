/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

// macOS-only capture path for the audio-thread watchdog. Two branches:
//
//   1. Unsandboxed hosts (Reaper, Ableton, Studio One, FL, Bitwig, ...):
//      thread_suspend -> thread_get_state -> immediate thread_resume ->
//      build a sentry_value_t event with the captured PC + frame-pointer
//      walk of the suspended thread -> sentry_capture_event.
//
//      CRITICAL: all sentry_value_new_* allocations happen AFTER
//      thread_resume, so we never hold the malloc lock while another
//      malloc-holding thread is suspended. This avoids the classic
//      lock-while-suspended deadlock.
//
//   2. Sandboxed AU hosts (Logic Pro, GarageBand, MainStage):
//      serialise a JSON file under $HOME/Library/Caches/PluginMakerPendingHangs/
//      The PendingDump module scans + uploads on the next launch.
//
// Sandbox detection uses an explicit allowlist of known sandboxed bundle
// IDs (com.apple.audio.* matches almost nothing in practice) PLUS a
// runtime probe via task_threads(mach_task_self()) which fails inside a
// real sandbox — belt and braces.
//
// The entire file body is gated on __APPLE__ so non-Apple builds get an
// empty TU and the CMakeLists' add_library() doesn't have to ifdef.

#if defined(__APPLE__)

#ifdef IPLUG_USE_SENTRY

#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_status.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <sentry.h>

namespace iplug
{
namespace sentry
{
namespace watchdog
{

// MUST match the named-namespace forward declaration in IPlugSentryWatchdog.cpp.
// External linkage — the .cpp calls into this from CaptureStall().
void CaptureStall_macOS(uint32_t audioMachThread,
                        std::chrono::milliseconds stallMs,
                        int32_t blockSize,
                        double sampleRate);

namespace
{
  // -------------------------------------------------------------------------
  // Sandbox detection. Logic Pro is com.apple.logic10 / com.apple.logic-pro;
  // GarageBand is com.apple.garageband10; MainStage is com.apple.mainstage3.
  // None of those have the legacy `com.apple.audio.` prefix. We allow-list
  // explicitly AND probe at runtime as a fallback.
  // -------------------------------------------------------------------------
  std::once_flag         gSandboxDetectOnce;
  bool                   gIsSandboxedAuHost = false;
  std::string            gHostBundleIdCached;

  bool BundleIdIndicatesSandboxedHost(NSString* bid)
  {
    if (!bid) return false;
    static NSString* const kHosts[] = {
      @"com.apple.logic10",
      @"com.apple.logic-pro",
      @"com.apple.logic.pro",
      @"com.apple.garageband10",
      @"com.apple.mainstage3",
    };
    for (NSString* h : kHosts)
      if ([bid isEqualToString:h]) return true;
    // Legacy/forward-compat prefix — some Apple AU host appex bundles use
    // com.apple.audio.* and we want to treat those as sandboxed too.
    if ([bid hasPrefix:@"com.apple.audio."]) return true;
    return false;
  }

  bool RuntimeProbeSandboxed()
  {
    // Inside a tight sandbox task_threads(mach_task_self(), ...) returns
    // KERN_NO_ACCESS or similar. Outside it succeeds and gives us our
    // thread list back — deallocate immediately so we don't leak ports.
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = task_threads(mach_task_self(), &list, &count);
    if (kr != KERN_SUCCESS) return true;
    for (mach_msg_type_number_t i = 0; i < count; ++i)
      mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t) list,
                  count * sizeof(thread_act_t));
    return false;
  }

  void DetectSandboxOnce()
  {
    std::call_once(gSandboxDetectOnce, []() {
      @autoreleasepool {
        NSBundle* main = [NSBundle mainBundle];
        NSString* bid = main ? [main bundleIdentifier] : nil;
        if (bid) gHostBundleIdCached = [bid UTF8String];
        if (BundleIdIndicatesSandboxedHost(bid)) gIsSandboxedAuHost = true;
        NSString* bundlePath = main ? [main bundlePath] : nil;
        if (bundlePath && [bundlePath hasSuffix:@".appex"])
          gIsSandboxedAuHost = true;
        if (!gIsSandboxedAuHost && RuntimeProbeSandboxed())
          gIsSandboxedAuHost = true;
      }
    });
  }

  // -------------------------------------------------------------------------
  // Pending-hangs directory under the (potentially appex-rewritten) $HOME.
  // Mirrored exactly in IPlugSentryPendingDump.cpp's scanner side.
  // -------------------------------------------------------------------------
  std::string PendingHangsDir()
  {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return "";
    std::string dir(home);
    dir += "/Library/Caches/PluginMakerPendingHangs";
    return dir;
  }

  // Use NSFileManager so intermediate directories (Library, Caches) are
  // created if missing — some appex containers don't auto-populate them.
  bool EnsureDirectoryExists(const std::string& dir)
  {
    if (dir.empty()) return false;
    @autoreleasepool {
      NSString* path = [NSString stringWithUTF8String:dir.c_str()];
      if (!path) return false;
      NSError* err = nil;
      BOOL ok = [[NSFileManager defaultManager]
                  createDirectoryAtPath:path
                  withIntermediateDirectories:YES
                  attributes:nil error:&err];
      return ok ? true : false;
    }
  }

  std::string MakeUuid()
  {
    uuid_t u;
    uuid_generate(u);
    char buf[37] = {0};
    uuid_unparse_lower(u, buf);
    return std::string(buf);
  }

  // ISO-8601 with millisecond precision so multiple events in the same
  // second can still be ordered correctly.
  std::string Iso8601Utc()
  {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tmv;
    gmtime_r(&tv.tv_sec, &tmv);
    char buf[40] = {0};
    int n = std::snprintf(buf, sizeof(buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                          tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                          tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                          (int) (tv.tv_usec / 1000));
    (void) n;
    return std::string(buf);
  }

  const char* SafePluginId()
  {
  #ifdef PLUGIN_ID
    return (PLUGIN_ID[0] != '\0') ? PLUGIN_ID : "unknown";
  #else
    return "unknown";
  #endif
  }
  const char* SafePluginVersion()
  {
  #ifdef PLUGIN_VERSION
    return (PLUGIN_VERSION[0] != '\0') ? PLUGIN_VERSION : "0";
  #else
    return "0";
  #endif
  }

  // Restrict bundle IDs to the ASCII reverse-DNS subset so the hand-rolled
  // JSON escape can stay simple. Anything outside [A-Za-z0-9._-] is dropped
  // — covers the entire known Apple host catalogue with room to spare.
  std::string SanitizeBundleId(const std::string& s)
  {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
          || (c >= '0' && c <= '9')
          || c == '.' || c == '_' || c == '-')
        out.push_back(c);
    }
    return out;
  }

  // Conservative JSON escape: quotes/backslashes, plus everything <0x20
  // becomes '?'. Inputs are limited to vetted strings (PLUGIN_ID,
  // PLUGIN_VERSION, sanitized bundle ID) so this is defense-in-depth.
  std::string JsonEscape(const std::string& s)
  {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s)
    {
      if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
      else if ((unsigned char) c < 0x20 || (unsigned char) c == 0x7F)
        out.push_back('?');
      else out.push_back(c);
    }
    return out;
  }

  // Count existing hangdump files in the directory; used to cap on-disk
  // accumulation so a chronic stall can't bloat Caches indefinitely.
  size_t CountExistingDumps(const std::string& dir)
  {
    size_t n = 0;
    @autoreleasepool {
      NSString* path = [NSString stringWithUTF8String:dir.c_str()];
      if (!path) return 0;
      NSArray<NSString*>* contents = [[NSFileManager defaultManager]
        contentsOfDirectoryAtPath:path error:nil];
      for (NSString* name in contents)
      {
        if ([name hasSuffix:@".hangdump"] ||
            [name hasSuffix:@".hangdump.uploading"])
          ++n;
      }
    }
    return n;
  }

  void WriteSandboxedHangdump(std::chrono::milliseconds stallMs,
                              int32_t blockSize, double sampleRate)
  {
    const std::string dir = PendingHangsDir();
    if (!EnsureDirectoryExists(dir)) return;

    // Cheap dir-size guard — don't let a chronic stall fill Caches.
    if (CountExistingDumps(dir) >= 50) return;

    const std::string path = dir + "/" + MakeUuid() + ".hangdump";

    const std::string hostId = SanitizeBundleId(gHostBundleIdCached);

    char body[1024];
    const int n = std::snprintf(body, sizeof(body),
      "{\"version\":1,\"timestamp\":\"%s\",\"stall_duration_ms\":%lld,"
      "\"block_size\":%d,\"sample_rate\":%.2f,\"host_bundle_id\":\"%s\","
      "\"plugin_id\":\"%s\",\"plugin_version\":\"%s\"}",
      Iso8601Utc().c_str(),
      (long long) stallMs.count(),
      blockSize, sampleRate,
      JsonEscape(hostId).c_str(),
      JsonEscape(SafePluginId()).c_str(),
      JsonEscape(SafePluginVersion()).c_str());
    if (n <= 0 || n >= (int) sizeof(body)) return;

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return;
    const size_t written = std::fwrite(body, 1, (size_t) n, fp);
    std::fclose(fp);
    if (written != (size_t) n)
    {
      // Partial write — disk full / quota exceeded. Bin the truncated
      // file so the scanner doesn't waste a budget slot on garbage next
      // launch.
      ::unlink(path.c_str());
    }
  }

  // -------------------------------------------------------------------------
  // Unsandboxed path. Suspend the audio thread JUST long enough to grab its
  // PC + FP, immediately resume, then walk frame pointers offline. All
  // sentry_value_new_* calls happen AFTER thread_resume so we cannot
  // deadlock against a malloc-holding suspended thread.
  // -------------------------------------------------------------------------
  bool AddressLooksSane(uintptr_t addr)
  {
    // Sanity floor: lowest user-space addresses on Darwin start well above
    // 4 KiB. Anything below is either zero or a corrupted FP.
    return addr >= 0x1000ULL;
  }

  void CaptureUnsandboxed(mach_port_t audioThread,
                          std::chrono::milliseconds stallMs,
                          int32_t blockSize,
                          double sampleRate)
  {
    if (audioThread == 0) return;
    // Refuse to suspend the watchdog thread itself.
    const mach_port_t self = pthread_mach_thread_np(pthread_self());
    if (audioThread == self) return;

    // Validate the port still names a live thread of our task. If the
    // audio thread already died (DAW recycled it on sample-rate change
    // before we got here), the port name may have been recycled.
    thread_basic_info_data_t basicInfo;
    mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
    if (thread_info(audioThread, THREAD_BASIC_INFO,
                    (thread_info_t) &basicInfo, &basicCount) != KERN_SUCCESS)
      return;

    if (thread_suspend(audioThread) != KERN_SUCCESS) return;

    // Capture register state into a stack buffer, resume immediately.
    uintptr_t framePcs[64];
    int nframes = 0;

  #if defined(__aarch64__)
    arm_thread_state64_t state = {};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(audioThread, ARM_THREAD_STATE64,
                                        (thread_state_t) &state, &count);
    thread_resume(audioThread);
    if (kr == KERN_SUCCESS)
    {
      // arm64 user-space PC
      uintptr_t pc = (uintptr_t) __darwin_arm_thread_state64_get_pc(state);
      uintptr_t fp = (uintptr_t) __darwin_arm_thread_state64_get_fp(state);
      if (AddressLooksSane(pc))
        framePcs[nframes++] = pc;
      // Walk frame pointers: fp[0] = saved fp, fp[1] = saved lr.
      // Bound at 64 frames, sanity-check every fp.
      uintptr_t lastFp = 0;
      while (nframes < 64 && AddressLooksSane(fp) && fp != lastFp)
      {
        uintptr_t* fpPtr = reinterpret_cast<uintptr_t*>(fp);
        uintptr_t nextFp = fpPtr[0];
        uintptr_t lr     = fpPtr[1];
        if (AddressLooksSane(lr)) framePcs[nframes++] = lr;
        if (nextFp <= fp) break; // stack grows down; fp must increase
        lastFp = fp;
        fp = nextFp;
      }
    }
  #elif defined(__x86_64__)
    x86_thread_state64_t state = {};
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(audioThread, x86_THREAD_STATE64,
                                        (thread_state_t) &state, &count);
    thread_resume(audioThread);
    if (kr == KERN_SUCCESS)
    {
      uintptr_t pc = (uintptr_t) state.__rip;
      uintptr_t fp = (uintptr_t) state.__rbp;
      if (AddressLooksSane(pc))
        framePcs[nframes++] = pc;
      uintptr_t lastFp = 0;
      while (nframes < 64 && AddressLooksSane(fp) && fp != lastFp)
      {
        uintptr_t* fpPtr = reinterpret_cast<uintptr_t*>(fp);
        uintptr_t savedFp = fpPtr[0];
        uintptr_t retAddr = fpPtr[1];
        if (AddressLooksSane(retAddr)) framePcs[nframes++] = retAddr;
        if (savedFp <= fp) break;
        lastFp = fp;
        fp = savedFp;
      }
    }
  #else
    thread_resume(audioThread);
  #endif

    // Now safe to malloc — audio thread is running again.
    sentry_value_t event = sentry_value_new_event();
    sentry_value_set_by_key(event, "level", sentry_value_new_string("warning"));
    sentry_value_set_by_key(event, "logger",
      sentry_value_new_string("iplug.watchdog"));
    sentry_value_set_by_key(event, "message",
      sentry_value_new_string("iPlug2 watchdog detected stalled ProcessBlock"));

    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "event_type",
      sentry_value_new_string("audio_thread_hang"));
    char stallBuf[32];
    std::snprintf(stallBuf, sizeof(stallBuf), "%lld",
      (long long) stallMs.count());
    sentry_value_set_by_key(tags, "stall_duration_ms",
      sentry_value_new_string(stallBuf));
    sentry_value_set_by_key(event, "tags", tags);

    sentry_value_t extra = sentry_value_new_object();
    sentry_value_set_by_key(extra, "stall_duration_ms",
      sentry_value_new_int32((int32_t) stallMs.count()));
    sentry_value_set_by_key(extra, "block_size",
      sentry_value_new_int32(blockSize));
    sentry_value_set_by_key(extra, "sample_rate",
      sentry_value_new_double(sampleRate));
    sentry_value_set_by_key(event, "extra", extra);

    sentry_value_t frames = sentry_value_new_list();
    for (int i = nframes - 1; i >= 0; --i)
    {
      // Sentry expects frames bottom-up.
      char addrBuf[32];
      std::snprintf(addrBuf, sizeof(addrBuf), "0x%llx",
        (unsigned long long) framePcs[i]);
      sentry_value_t frame = sentry_value_new_object();
      sentry_value_set_by_key(frame, "instruction_addr",
        sentry_value_new_string(addrBuf));
      sentry_value_append(frames, frame);
    }

    sentry_value_t stacktrace = sentry_value_new_object();
    sentry_value_set_by_key(stacktrace, "frames", frames);

    sentry_value_t thread = sentry_value_new_object();
    sentry_value_set_by_key(thread, "id",
      sentry_value_new_int32((int32_t) audioThread));
    sentry_value_set_by_key(thread, "name",
      sentry_value_new_string("audio"));
    sentry_value_set_by_key(thread, "crashed", sentry_value_new_bool(0));
    sentry_value_set_by_key(thread, "stacktrace", stacktrace);

    sentry_value_t threads = sentry_value_new_list();
    sentry_value_append(threads, thread);
    sentry_value_set_by_key(event, "threads", threads);

    sentry_capture_event(event);
  }
} // anonymous namespace

void CaptureStall_macOS(uint32_t audioMachThread,
                        std::chrono::milliseconds stallMs,
                        int32_t blockSize,
                        double sampleRate)
{
  DetectSandboxOnce();
  if (gIsSandboxedAuHost)
  {
    WriteSandboxedHangdump(stallMs, blockSize, sampleRate);
    return;
  }
  CaptureUnsandboxed(static_cast<mach_port_t>(audioMachThread),
                     stallMs, blockSize, sampleRate);
}

} // namespace watchdog
} // namespace sentry
} // namespace iplug

#endif // IPLUG_USE_SENTRY

#endif // __APPLE__
