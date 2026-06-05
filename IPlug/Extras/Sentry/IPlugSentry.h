/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#pragma once

/**
 * @file
 * @brief Optional Sentry Native crash + perf telemetry for iPlug2 plugins.
 *
 * Compiled in only when the build defines IPLUG_USE_SENTRY (CMake option of
 * the same name). When IPLUG_USE_SENTRY is not defined, every Init() call
 * becomes a no-op — no Sentry symbols, no extra binary size, no risk.
 *
 * Activation contract:
 *   - The plugin's build must also define SENTRY_DSN as a C string literal
 *     (typically via `-DSENTRY_DSN="https://..."` from the build worker).
 *     If SENTRY_DSN is empty, Init() returns without calling sentry_init.
 *   - PLUGIN_ID and PLUGIN_VERSION should be defined the same way so
 *     events are tagged correctly. Both are forwarded as Sentry tags +
 *     used to construct the release identifier
 *     `${PLUGIN_ID}@${PLUGIN_VERSION}`.
 *
 * Consent contract (Surface A — vendor-wide sentinel):
 *   - Init() reads a JSON sentinel from the user's data dir before doing
 *     anything. If the file is missing, malformed, or `accepted` is not
 *     true, sentry_init is never called.
 *   - Windows: %APPDATA%\PluginMaker\telemetry_consent.json
 *   - macOS:   ~/Library/Application Support/PluginMaker/telemetry_consent.json
 *   - File shape (v1): {"version":1,"accepted":true,...}
 *   - Schema is versioned: newer versions are trusted forward (no
 *     downgrade flip-flop), older versions return "unset" so the user
 *     re-consents under the broader scope. Legacy {"telemetry":bool}
 *     is honored asymmetrically — `false` is preserved as declined,
 *     `true` returns "unset" (re-prompt under broader scope, per EDPB
 *     Guidelines 5/2020 on consent specificity).
 *   - Sentinel is written by the installer (see installer-electron) and
 *     covers both installer + every PluginMaker plugin under one decision.
 *
 * Known gap — sandboxed AUs (Logic Pro, GarageBand):
 *   Inside a sandboxed AU host $HOME is rewritten to the per-host appex
 *   container, so the path above cannot reach the installer's global
 *   Application Support file. Resolving this requires either (a) an
 *   app-group entitlement shared between the installer and every
 *   supported AU host, or (b) the installer writing per-host container
 *   copies. Both are installer + ops work. Until that lands, the gate
 *   safely returns "no consent" inside sandboxed AUs and Sentry stays
 *   off there — the right default when consent is unknown.
 *
 * Threading:
 *   - Init() is safe to call from any thread but must NOT be called from
 *     the audio thread (sentry_init does allocations + opens files).
 *   - Internally guarded by std::call_once. The once_flag is per-DLL: a
 *     single PluginMaker plugin with 32 instances in one project calls
 *     sentry_init exactly once. Two DISTINCT PluginMaker plugins loaded
 *     into the same DAW process each have their own once_flag and each
 *     call sentry_init separately — sentry-native's own re-init guard
 *     handles the second call (the second init's options are ignored;
 *     the first plugin's DSN + database path win). Per-process dedupe
 *     across distinct plugins would need a named-semaphore or env-var
 *     sentinel; tracked as a separate hardening item.
 */

namespace iplug
{
namespace sentry
{

  /** Initialise Sentry Native exactly once for the current process.
   *
   *  Safe to call from every plugin instance constructor — only the first
   *  call performs work. No-op when:
   *    - IPLUG_USE_SENTRY is not defined at compile time;
   *    - SENTRY_DSN is empty;
   *    - The consent sentinel is missing or not-accepted.
   *
   *  @param pluginId       Stable plugin identifier (passed in as
   *                        compile-time macro PLUGIN_ID by the build).
   *                        Used as a Sentry tag + part of the release.
   *  @param pluginVersion  Plugin version string ("83", "2.10.1", etc.).
   *                        Used as a Sentry tag + part of the release.
   *
   *  When IPLUG_USE_SENTRY is not defined the function is declared inline
   *  with an empty body so every translation unit that includes this
   *  header gets a definition locally. That removes the requirement on
   *  consumers (e.g. legacy .vcxproj MSBuild targets) to compile and link
   *  IPlugSentry.cpp — they call an inline no-op instead of an external
   *  symbol that nobody provides.
   */
#ifdef IPLUG_USE_SENTRY
  void Init(const char* pluginId, const char* pluginVersion);
#else
  inline void Init(const char* /*pluginId*/, const char* /*pluginVersion*/) {}
#endif

} // namespace sentry
} // namespace iplug
