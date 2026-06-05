/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#include "IPlugSentryPendingDump.h"

#ifdef IPLUG_USE_SENTRY

  #include <algorithm>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <ctime>
  #include <string>
  #include <thread>
  #include <vector>

  #include <sentry.h>

  #if defined(__APPLE__)
    #include <dirent.h>
    #include <sys/stat.h>
    #include <sys/syslimits.h>
    #include <unistd.h>
  #elif defined(_WIN32)
    #include <windows.h>
  #endif

namespace iplug
{
namespace sentry
{
namespace pendingdump
{

namespace
{
  // Hard cap per launch.
  constexpr size_t kMaxUploadsPerLaunch = 5;
  // 30-day retention. Older dumps are dead data — the signal is stale.
  constexpr time_t kMaxAgeSeconds = 30LL * 24 * 60 * 60;
  // 8 KiB ceiling on individual file reads (writer emits ~256 bytes).
  constexpr size_t kMaxFileBytes = 8192;

  std::string PendingHangsDir()
  {
  #if defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home || !*home) return "";
    std::string dir(home);
    dir += "/Library/Caches/PluginMakerPendingHangs";
    return dir;
  #elif defined(_WIN32)
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (!localApp || !*localApp) return "";
    std::string dir(localApp);
    dir += "\\PluginMaker\\PendingHangs";
    return dir;
  #else
    return "";
  #endif
  }

  // ---------------------------------------------------------------------------
  // Hand-rolled tolerant JSON readers — same style as the sentinel reader in
  // IPlugSentry.cpp. Avoids linking a full JSON parser for one tiny shape.
  // ---------------------------------------------------------------------------
  void SkipWs(const std::string& s, size_t& pos)
  {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'
                              || s[pos] == '\n' || s[pos] == '\r')) ++pos;
  }

  size_t FindValueStart(const std::string& s, const char* key)
  {
    std::string needle("\"");
    needle += key;
    needle += "\"";
    size_t k = s.find(needle);
    if (k == std::string::npos) return std::string::npos;
    size_t colon = s.find(':', k + needle.size());
    if (colon == std::string::npos) return std::string::npos;
    size_t p = colon + 1;
    SkipWs(s, p);
    return p;
  }

  bool ReadStringAt(const std::string& s, size_t pos, std::string& out)
  {
    if (pos == std::string::npos || pos >= s.size() || s[pos] != '"') return false;
    size_t end = ++pos;
    while (end < s.size() && s[end] != '"')
    {
      if (s[end] == '\\' && end + 1 < s.size()) ++end;
      ++end;
    }
    if (end >= s.size()) return false;
    out.assign(s, pos, end - pos);
    return true;
  }

  bool ReadLongAt(const std::string& s, size_t pos, long long& out)
  {
    if (pos == std::string::npos || pos >= s.size()) return false;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str() + pos, &end, 10);
    if (end == s.c_str() + pos) return false;
    out = v;
    return true;
  }

  bool ReadDoubleAt(const std::string& s, size_t pos, double& out)
  {
    if (pos == std::string::npos || pos >= s.size()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str() + pos, &end);
    if (end == s.c_str() + pos) return false;
    out = v;
    return true;
  }

  std::string ReadFileSafe(const std::string& path)
  {
  #if defined(__APPLE__)
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "";
    if (!S_ISREG(st.st_mode)) return "";
    if (st.st_size <= 0 || (size_t) st.st_size > kMaxFileBytes) return "";
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return "";
    std::string out;
    out.resize((size_t) st.st_size);
    size_t n = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    if (n != out.size()) return "";
    return out;
  #elif defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0
        || size.QuadPart > (LONGLONG) kMaxFileBytes)
    {
      CloseHandle(h);
      return "";
    }
    std::string out;
    out.resize((size_t) size.QuadPart);
    DWORD readBytes = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD) out.size(), &readBytes, nullptr);
    CloseHandle(h);
    if (!ok || readBytes != out.size()) return "";
    return out;
  #else
    (void) path;
    return "";
  #endif
  }

  bool RenameToUploading(const std::string& path, std::string& outNewPath)
  {
    outNewPath = path + ".uploading";
  #if defined(__APPLE__)
    if (::rename(path.c_str(), outNewPath.c_str()) != 0) return false;
  #elif defined(_WIN32)
    if (!MoveFileExA(path.c_str(), outNewPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) return false;
  #else
    return false;
  #endif
    return true;
  }

  void DeleteFileSafe(const std::string& path)
  {
  #if defined(__APPLE__)
    ::unlink(path.c_str());
  #elif defined(_WIN32)
    DeleteFileA(path.c_str());
  #else
    (void) path;
  #endif
  }

  time_t FileMtime(const std::string& path)
  {
  #if defined(__APPLE__)
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_mtime;
  #elif defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER ull;
    ull.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    ull.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    return (time_t) ((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
  #else
    (void) path;
    return 0;
  #endif
  }

  bool FileIsTooOld(const std::string& path)
  {
    const time_t mtime = FileMtime(path);
    if (mtime == 0) return false;
    const time_t now = time(nullptr);
    return (now - mtime) > kMaxAgeSeconds;
  }

  struct DumpFile
  {
    std::string path;
    time_t      mtime;
    bool        isUploading; // previously-handed-off file from a prior launch
  };

  std::vector<DumpFile> ListHangdumpFiles(const std::string& dir)
  {
    std::vector<DumpFile> out;
  #if defined(__APPLE__)
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d))
    {
      const char* name = e->d_name;
      if (!name || name[0] == '.') continue;
      const size_t len = std::strlen(name);
      const char* kSuffix = ".hangdump";
      const char* kUpSuffix = ".hangdump.uploading";
      const size_t suffixLen = std::strlen(kSuffix);
      const size_t upSuffixLen = std::strlen(kUpSuffix);
      bool isUploading = false;
      bool match = false;
      if (len > upSuffixLen && std::strcmp(name + len - upSuffixLen, kUpSuffix) == 0)
      { match = true; isUploading = true; }
      else if (len > suffixLen && std::strcmp(name + len - suffixLen, kSuffix) == 0)
      { match = true; isUploading = false; }
      if (!match) continue;
      DumpFile df;
      df.path = dir + "/" + name;
      df.mtime = FileMtime(df.path);
      df.isUploading = isUploading;
      out.push_back(std::move(df));
    }
    closedir(d);
  #elif defined(_WIN32)
    std::string pattern = dir + "\\*.hangdump*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
      const char* name = fd.cFileName;
      const size_t len = std::strlen(name);
      const char* kSuffix = ".hangdump";
      const char* kUpSuffix = ".hangdump.uploading";
      const size_t suffixLen = std::strlen(kSuffix);
      const size_t upSuffixLen = std::strlen(kUpSuffix);
      bool isUploading = false;
      bool match = false;
      if (len > upSuffixLen && _stricmp(name + len - upSuffixLen, kUpSuffix) == 0)
      { match = true; isUploading = true; }
      else if (len > suffixLen && _stricmp(name + len - suffixLen, kSuffix) == 0)
      { match = true; isUploading = false; }
      if (match)
      {
        DumpFile df;
        df.path = dir + "\\" + name;
        df.mtime = FileMtime(df.path);
        df.isUploading = isUploading;
        out.push_back(std::move(df));
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  #else
    (void) dir;
  #endif
    return out;
  }

  bool UploadDump(const std::string& raw)
  {
    std::string timestamp;
    std::string hostBundleId;
    std::string pluginId;
    std::string pluginVersion;
    long long stallMs = 0;
    long long blockSize = 0;
    double sampleRate = 0.0;

    (void) ReadStringAt(raw, FindValueStart(raw, "timestamp"), timestamp);
    (void) ReadStringAt(raw, FindValueStart(raw, "host_bundle_id"), hostBundleId);
    (void) ReadStringAt(raw, FindValueStart(raw, "plugin_id"), pluginId);
    (void) ReadStringAt(raw, FindValueStart(raw, "plugin_version"), pluginVersion);
    (void) ReadLongAt(raw, FindValueStart(raw, "stall_duration_ms"), stallMs);
    (void) ReadLongAt(raw, FindValueStart(raw, "block_size"), blockSize);
    (void) ReadDoubleAt(raw, FindValueStart(raw, "sample_rate"), sampleRate);

    // Stricter shape check: require timestamp + (plugin_id OR host_bundle_id).
    // A garbled file that happens to contain `"stall_duration_ms":1` should
    // not produce an event with empty everything-else.
    if (timestamp.empty()) return false;
    if (pluginId.empty() && hostBundleId.empty()) return false;

    sentry_value_t event = sentry_value_new_event();
    sentry_value_set_by_key(event, "level", sentry_value_new_string("warning"));
    sentry_value_set_by_key(event, "logger",
      sentry_value_new_string("iplug.watchdog.pending"));
    sentry_value_set_by_key(event, "message", sentry_value_new_string(
      "iPlug2 watchdog: pending hangdump from prior launch"));
    sentry_value_set_by_key(event, "timestamp",
      sentry_value_new_string(timestamp.c_str()));

    // Use the dump's plugin id+version as the release tag so Discover
    // attributes the hang to the version that actually hung (not the
    // currently-running version).
    if (!pluginId.empty())
    {
      std::string release = pluginId;
      release += "@";
      release += pluginVersion.empty() ? "0" : pluginVersion;
      sentry_value_set_by_key(event, "release",
        sentry_value_new_string(release.c_str()));
    }

    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "event_type",
      sentry_value_new_string("audio_thread_hang"));
    sentry_value_set_by_key(tags, "capture_mode",
      sentry_value_new_string("pending_dump"));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld", stallMs);
    sentry_value_set_by_key(tags, "stall_duration_ms", sentry_value_new_string(buf));
    std::snprintf(buf, sizeof(buf), "%lld", blockSize);
    sentry_value_set_by_key(tags, "block_size", sentry_value_new_string(buf));
    std::snprintf(buf, sizeof(buf), "%.2f", sampleRate);
    sentry_value_set_by_key(tags, "sample_rate", sentry_value_new_string(buf));
    if (!hostBundleId.empty())
      sentry_value_set_by_key(tags, "host_bundle_id",
        sentry_value_new_string(hostBundleId.c_str()));
    if (!pluginId.empty())
      sentry_value_set_by_key(tags, "dumped_plugin_id",
        sentry_value_new_string(pluginId.c_str()));
    if (!pluginVersion.empty())
      sentry_value_set_by_key(tags, "dumped_plugin_version",
        sentry_value_new_string(pluginVersion.c_str()));
    sentry_value_set_by_key(event, "tags", tags);

    sentry_value_t extra = sentry_value_new_object();
    sentry_value_set_by_key(extra, "stall_duration_ms",
      sentry_value_new_int32((int32_t) stallMs));
    sentry_value_set_by_key(extra, "block_size",
      sentry_value_new_int32((int32_t) blockSize));
    sentry_value_set_by_key(extra, "sample_rate",
      sentry_value_new_double(sampleRate));
    sentry_value_set_by_key(event, "extra", extra);

    sentry_capture_event(event);
    return true;
  }

  void RunScanInline()
  {
    const std::string dir = PendingHangsDir();
    if (dir.empty()) return;

    std::vector<DumpFile> files = ListHangdumpFiles(dir);
    if (files.empty()) return;

    // Newest-first so the most actionable signal is uploaded under the cap.
    std::sort(files.begin(), files.end(),
              [](const DumpFile& a, const DumpFile& b) { return a.mtime > b.mtime; });

    size_t uploaded = 0;
    for (const DumpFile& df : files)
    {
      // Stale files: bin without uploading or counting against the cap.
      if (FileIsTooOld(df.path))
      {
        DeleteFileSafe(df.path);
        continue;
      }
      // .uploading files are leftovers from a prior launch that crashed
      // between rename and unlink — sentry-native's local DB already has
      // the event queued, so don't re-upload.
      if (df.isUploading)
      {
        DeleteFileSafe(df.path);
        continue;
      }
      if (uploaded >= kMaxUploadsPerLaunch) break;

      // Two-phase delete: rename, capture, delete. A crash anywhere here
      // leaves a `.uploading` file that the NEXT launch deletes without
      // re-uploading — idempotent against mid-step crash.
      std::string upPath;
      if (!RenameToUploading(df.path, upPath))
      {
        // Couldn't rename — try to read the original anyway and unlink.
        const std::string raw = ReadFileSafe(df.path);
        if (!raw.empty()) (void) UploadDump(raw);
        DeleteFileSafe(df.path);
        ++uploaded;
        continue;
      }
      const std::string raw = ReadFileSafe(upPath);
      if (!raw.empty()) (void) UploadDump(raw);
      DeleteFileSafe(upPath);
      ++uploaded;
    }
  }
} // anonymous namespace

void ScanAndUploadPendingDumps()
{
  // Run on a detached background thread so plugin construction stays snappy
  // — host probe-loads care about the first few ms of Init().
  try
  {
    std::thread(&RunScanInline).detach();
  }
  catch (...)
  {
    // Thread creation failed (OOM / process thread limit). Fall back to
    // running inline — worst case adds ~50-150ms to plugin Init, which
    // is better than silently never draining the queue.
    try { RunScanInline(); } catch (...) {}
  }
}

} // namespace pendingdump
} // namespace sentry
} // namespace iplug

#else // !IPLUG_USE_SENTRY

// Intentionally empty. Header's #else branch provides inline no-op.

#endif // IPLUG_USE_SENTRY
