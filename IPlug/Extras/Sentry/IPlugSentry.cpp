/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#include "IPlugSentry.h"

// All non-trivial code is gated behind IPLUG_USE_SENTRY. When the option is
// off, the symbol is a single no-op function with no Sentry headers ever
// included, no sentry-native library linked. Keep the surface tiny.
#ifdef IPLUG_USE_SENTRY

  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <mutex>
  #include <string>

  #include <sentry.h>

  #if defined(_WIN32)
    #include <windows.h>
    #include <shlobj.h>
    #include <psapi.h>
  #elif defined(__APPLE__)
    #include <dlfcn.h>
    #include <mach-o/dyld.h>
    #include <sys/stat.h>
    #include <sys/syslimits.h>
    #include <unistd.h>
  #endif

namespace iplug
{
namespace sentry
{

namespace
{
  // ---------------------------------------------------------------------------
  // Sentinel reader — mirrors the contract written by installer-electron's
  // consentSentinel.ts. Kept deliberately defensive: any read or parse
  // failure → return false → Sentry stays uninitialised.
  // ---------------------------------------------------------------------------

  // Cap reads at 4 KiB. Sentinel is ~150 bytes in practice; anything larger
  // is either corrupt or hostile and would block plugin construction.
  // Matches consentSentinel.ts:MAX_SENTINEL_BYTES.
  constexpr size_t kMaxSentinelBytes = 4096;

  // Schema version recognised here. Anything > this is trusted forward; <
  // means scope likely expanded since the user decided, so we re-prompt
  // (i.e. treat as unset → init never fires). Matches consentSentinel.ts:
  // SENTINEL_VERSION.
  constexpr int kSentinelVersion = 1;

  // Sentinel path (single, vendor-wide).
  //
  // Sandboxed AU hosts (Logic Pro, GarageBand) are a known gap: inside the
  // sandbox $HOME is rewritten to point at the per-host appex container,
  // so neither this path nor any naive computation can reach the installer's
  // global Application Support file. Resolving that requires either (a) an
  // app-group entitlement shared between the installer and every supported
  // AU host, or (b) the installer writing per-host container copies. Both
  // are installer + ops work, not framework work. Until then the consent
  // gate returns false inside sandboxed AUs and Sentry stays off there —
  // safe-by-default, exactly the behavior we want when consent is unknown.
  std::string SentinelPath()
  {
  #if defined(_WIN32)
    PWSTR appData = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData);
    // CoTaskMemFree accepts nullptr per MSDN — unconditional free is the
    // safest pattern even though the API contract says no allocation on
    // failure. Defends against pre-Vista quirks + future regressions.
    if (FAILED(hr)) { CoTaskMemFree(appData); return ""; }
    char buf[MAX_PATH] = {0};
    WideCharToMultiByte(CP_UTF8, 0, appData, -1, buf, MAX_PATH, nullptr, nullptr);
    CoTaskMemFree(appData);
    std::string path(buf);
    path += "\\PluginMaker\\telemetry_consent.json";
    return path;
  #elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home || !*home) return "";
    std::string path(home);
    path += "/Library/Application Support/PluginMaker/telemetry_consent.json";
    return path;
  #else
    return "";
  #endif
  }

  // Returns the file content if it exists, is a regular file, and is small
  // enough to be a legitimate sentinel. Empty string otherwise.
  std::string ReadSentinelSafe(const std::string& path)
  {
    if (path.empty()) return "";
  #if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (LONGLONG) kMaxSentinelBytes)
    {
      CloseHandle(h);
      return "";
    }
    std::string out;
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD readBytes = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD) out.size(), &readBytes, nullptr);
    CloseHandle(h);
    if (!ok || readBytes != out.size()) return "";
    return out;
  #else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "";
    if (!S_ISREG(st.st_mode)) return "";
    if (st.st_size <= 0 || (size_t) st.st_size > kMaxSentinelBytes) return "";
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return "";
    std::string out;
    out.resize(static_cast<size_t>(st.st_size));
    size_t n = std::fread(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    if (n != out.size()) return "";
    return out;
  #endif
  }

  // Hand-rolled minimal parser. The sentinel is a fixed-shape ~150-byte
  // JSON written by consentSentinel.ts; only three keys ever matter
  // (`version`, `accepted`, `telemetry` for the legacy shape). We do not
  // link a full JSON parser to avoid a dependency for one boolean.
  //
  // Mirrors consentSentinel.ts:parseSentinel semantics — including the
  // GDPR-aware legacy migration: legacy `telemetry:false` is preserved
  // as Declined, but legacy `telemetry:true` returns Unset (re-prompt
  // under the broader new scope, per EDPB Guidelines 5/2020 on consent
  // specificity).
  enum class ConsentDecision { Unset, Accepted, Declined };

  // Skip whitespace (space, tab, newline, CR) starting at `pos`.
  void SkipWs(const std::string& s, size_t& pos)
  {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
      ++pos;
  }

  // Locate `"key"` then `:` then skip whitespace. Returns the index of the
  // first non-whitespace char after the colon, or npos if not found.
  size_t FindValueStart(const std::string& s, const char* key)
  {
    std::string needle("\"");
    needle += key;
    needle += "\"";
    size_t k = s.find(needle);
    if (k == std::string::npos) return std::string::npos;
    size_t colon = s.find(':', k + needle.size());
    if (colon == std::string::npos) return std::string::npos;
    size_t pos = colon + 1;
    SkipWs(s, pos);
    return pos;
  }

  bool ReadBoolAt(const std::string& s, size_t pos, bool& out)
  {
    if (pos == std::string::npos || pos >= s.size()) return false;
    if (s.compare(pos, 4, "true") == 0)  { out = true;  return true; }
    if (s.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
  }

  bool ReadIntAt(const std::string& s, size_t pos, long& out)
  {
    if (pos == std::string::npos || pos >= s.size()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str() + pos, &end, 10);
    if (end == s.c_str() + pos) return false;
    out = v;
    return true;
  }

  ConsentDecision ParseSentinelDecision(const std::string& raw)
  {
    if (raw.empty()) return ConsentDecision::Unset;

    long version = 0;
    bool accepted = false;
    bool hasV1 = ReadIntAt(raw, FindValueStart(raw, "version"), version)
              && ReadBoolAt(raw, FindValueStart(raw, "accepted"), accepted);

    if (hasV1)
    {
      // Newer than us — trust the decision but don't rewrite. Mirrors
      // consentSentinel.ts:128-134 (no-downgrade-flip-flop protection).
      if (version > kSentinelVersion)
        return accepted ? ConsentDecision::Accepted : ConsentDecision::Declined;
      // Older — scope likely expanded since the user decided.
      if (version < kSentinelVersion)
        return ConsentDecision::Unset;
      // Current — trust as written.
      return accepted ? ConsentDecision::Accepted : ConsentDecision::Declined;
    }

    // Legacy `{telemetry: bool}` — asymmetric migration.
    bool legacy = false;
    if (ReadBoolAt(raw, FindValueStart(raw, "telemetry"), legacy))
    {
      // Legacy false: preserved-no (consentSentinel.ts:161-163).
      // Legacy true:  Unset → re-prompt under broader scope (lines 154-159).
      return legacy ? ConsentDecision::Unset : ConsentDecision::Declined;
    }

    return ConsentDecision::Unset;
  }

  bool ConsentGranted()
  {
    const std::string raw = ReadSentinelSafe(SentinelPath());
    if (raw.empty()) return false;             // missing / oversized / unreadable
    return ParseSentinelDecision(raw) == ConsentDecision::Accepted;
  }

  // ---------------------------------------------------------------------------
  // Module-address filter for before_send. Drop events whose top frame is
  // NOT inside this plugin's loaded module — protects us from accidentally
  // capturing crashes that originate in other plugins / the host itself.
  // ---------------------------------------------------------------------------

  uintptr_t gModuleStart = 0;
  uintptr_t gModuleEnd = 0;

  void DetectModuleRange()
  {
  #if defined(_WIN32)
    HMODULE mod = nullptr;
    // Get the module that contains this function's address.
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&DetectModuleRange), &mod) && mod)
    {
      MODULEINFO info = {};
      if (GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info)))
      {
        gModuleStart = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        gModuleEnd = gModuleStart + info.SizeOfImage;
      }
    }
  #elif defined(__APPLE__)
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&DetectModuleRange), &info) && info.dli_fbase)
    {
      gModuleStart = reinterpret_cast<uintptr_t>(info.dli_fbase);
      // No clean way to get the slice end; use the next Mach-O image's start.
      // Best-effort: walk the dyld image list and find the slot containing
      // our base, then use the next slot's load address.
      const uint32_t count = _dyld_image_count();
      for (uint32_t i = 0; i < count; ++i)
      {
        const struct mach_header* hdr = _dyld_get_image_header(i);
        if (reinterpret_cast<uintptr_t>(hdr) == gModuleStart)
        {
          if (i + 1 < count)
          {
            gModuleEnd = reinterpret_cast<uintptr_t>(_dyld_get_image_header(i + 1));
          }
          else
          {
            // Last image in the list. Use a generous default.
            gModuleEnd = gModuleStart + (64 * 1024 * 1024);
          }
          break;
        }
      }
    }
  #endif
  }

  bool AddressInOurModule(uintptr_t addr)
  {
    if (gModuleStart == 0 || gModuleEnd == 0) return true; // unknown — don't filter
    return addr >= gModuleStart && addr < gModuleEnd;
  }

  sentry_value_t BeforeSendCallback(sentry_value_t event, void* /*hint*/, void* /*closure*/)
  {
    // Walk exception.values[*].stacktrace.frames to find the top in-app frame.
    // If no frame is inside our module, drop the event — we likely captured
    // someone else's crash because our handler happened to be registered.
    if (gModuleStart == 0 || gModuleEnd == 0) return event;

    sentry_value_t exception = sentry_value_get_by_key(event, "exception");
    if (sentry_value_is_null(exception)) return event;
    sentry_value_t values = sentry_value_get_by_key(exception, "values");
    if (sentry_value_is_null(values)) return event;

    bool ourFrameFound = false;
    const size_t valueCount = sentry_value_get_length(values);
    for (size_t i = 0; i < valueCount && !ourFrameFound; ++i)
    {
      sentry_value_t v = sentry_value_get_by_index(values, i);
      sentry_value_t st = sentry_value_get_by_key(v, "stacktrace");
      if (sentry_value_is_null(st)) continue;
      sentry_value_t frames = sentry_value_get_by_key(st, "frames");
      if (sentry_value_is_null(frames)) continue;
      const size_t frameCount = sentry_value_get_length(frames);
      for (size_t f = 0; f < frameCount; ++f)
      {
        sentry_value_t fr = sentry_value_get_by_index(frames, f);
        sentry_value_t addrVal = sentry_value_get_by_key(fr, "instruction_addr");
        if (sentry_value_is_null(addrVal)) continue;
        const char* addrStr = sentry_value_as_string(addrVal);
        if (!addrStr) continue;
        // Sentry sends addresses as "0x..." hex strings.
        uintptr_t addr = static_cast<uintptr_t>(std::strtoull(addrStr, nullptr, 0));
        if (AddressInOurModule(addr))
        {
          ourFrameFound = true;
          break;
        }
      }
    }

    if (!ourFrameFound)
    {
      sentry_value_decref(event);
      return sentry_value_new_null();
    }
    return event;
  }

  // ---------------------------------------------------------------------------
  // Database path — Sentry-native needs a writable scratch dir for its
  // local event queue. Use a per-plugin subdir of the vendor data path so
  // multiple plugins don't fight over the same dir.
  // ---------------------------------------------------------------------------

  std::string DatabasePath(const char* pluginId)
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
    path += pluginId ? pluginId : "unknown";
    return path;
  #elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home || !*home) return "";
    std::string path(home);
    path += "/Library/Application Support/PluginMaker/sentry-db/";
    path += pluginId ? pluginId : "unknown";
    return path;
  #else
    return "";
  #endif
  }

} // anonymous namespace

void Init(const char* pluginId, const char* pluginVersion)
{
  static std::once_flag sInitOnce;
  std::call_once(sInitOnce, [pluginId, pluginVersion]() {

    // Compile-time DSN provided by the build worker. Empty in dev / unconfigured
    // builds — bail silently.
  #ifndef SENTRY_DSN
    return;
  #else
    constexpr const char* kDsn = SENTRY_DSN;
    if (!kDsn || !*kDsn) return;
  #endif

    if (!ConsentGranted()) return;

    DetectModuleRange();

    sentry_options_t* options = sentry_options_new();
    sentry_options_set_dsn(options, kDsn);

    // Use Crashpad backend on Windows (out-of-process helper, survives
    // every-thread-wedged scenarios). Use Breakpad backend on macOS
    // (in-process; Crashpad's helper cannot spawn under Logic's AU sandbox).
  #if defined(_WIN32)
    // Crashpad needs an explicit handler-path; sentry-native bundles it via
    // FetchContent and the build pipeline copies it to the plugin bundle's
    // Helpers dir. Path is resolved at build time relative to the plugin.
    // If the file is missing at runtime sentry_init still succeeds but
    // crash reporting silently degrades — log a warning.
    sentry_options_set_handler_path(options,
      "crashpad_handler.exe"); // expected next to the plugin DLL
  #endif

    const std::string dbPath = DatabasePath(pluginId);
    if (!dbPath.empty())
      sentry_options_set_database_path(options, dbPath.c_str());

    // Release tag: "<pluginId>@<pluginVersion>". The build worker may also
    // append "+<gitSha>" in a later PR — keep the format extensible.
    std::string release;
    release += (pluginId && *pluginId) ? pluginId : "unknown";
    release += "@";
    release += (pluginVersion && *pluginVersion) ? pluginVersion : "0";
    sentry_options_set_release(options, release.c_str());

    sentry_options_set_environment(options, "production");
    sentry_options_set_auto_session_tracking(options, 0);
    sentry_options_set_max_breadcrumbs(options, 50);
    sentry_options_set_sample_rate(options, 1.0); // capture every crash
    sentry_options_set_traces_sample_rate(options, 0.0); // no perf traces
    sentry_options_set_before_send(options, BeforeSendCallback, nullptr);

    if (sentry_init(options) != 0)
    {
      // Init failed (couldn't write database, helper missing, etc.).
      // No retry — silent degradation, never blocks the plugin.
      return;
    }

    // Tags. Plugins flow through the module-address filter, so we know
    // every event with these tags genuinely came from us.
    //
    // Two come from the runtime args (pluginId, pluginVersion). The other
    // two come from compile-time -D flags baked at build time:
    //   -DBRAND_SLUG="<slug>"          from brand.json (or "pluginmaker"
    //                                  for the consumer flow per the
    //                                  alexh worker's safeCmakeDefineValue)
    //   -DGENERATED_BY_PROMPT="<hash>" 12-char sha256 prefix; backend bakes
    //                                  it so we never ship the prompt text
    //                                  verbatim through Sentry.
    // Both are gated on macro existence + non-empty content so older
    // pipelines that don't define them just skip the tag instead of
    // sending "(null)" / "".
    sentry_set_tag("pluginId", (pluginId && *pluginId) ? pluginId : "unknown");
    sentry_set_tag("pluginVersion",
                   (pluginVersion && *pluginVersion) ? pluginVersion : "0");
  #ifdef BRAND_SLUG
    {
      constexpr const char* kBrandSlug = BRAND_SLUG;
      if (kBrandSlug && *kBrandSlug)
        sentry_set_tag("brandSlug", kBrandSlug);
    }
  #endif
  #ifdef GENERATED_BY_PROMPT
    {
      constexpr const char* kGeneratedByPrompt = GENERATED_BY_PROMPT;
      if (kGeneratedByPrompt && *kGeneratedByPrompt)
        sentry_set_tag("generatedByPrompt", kGeneratedByPrompt);
    }
  #endif
  });
}

} // namespace sentry
} // namespace iplug

#else // !IPLUG_USE_SENTRY

// Intentionally empty. The no-op stub for iplug::sentry::Init lives in
// IPlugSentry.h as an `inline` definition in the matching #else branch,
// so every translation unit that includes the header — including this
// one and the MSBuild paths that never compile this .cpp — already has
// a usable definition. Re-defining it here non-inline triggered a
// redefinition error when CMake builds this .cpp as a static lib with
// IPLUG_USE_SENTRY=OFF (the default), which broke FindiPlug2.cmake's
// unconditional `add_subdirectory(IPlug/Extras/Sentry)`.

#endif // IPLUG_USE_SENTRY
