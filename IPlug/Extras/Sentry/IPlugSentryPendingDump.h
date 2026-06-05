/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for more info.

 ==============================================================================
*/

#pragma once

/**
 * @file
 * @brief Drain + upload watchdog hangdump files from previous launches.
 *
 * Inside sandboxed AU hosts (Logic Pro, GarageBand, MainStage) the watchdog
 * cannot safely use `mach_thread_suspend` + state-walk to inspect the audio
 * thread in-process — Mach exceptions get intercepted by the host and
 * holding the malloc lock across a sentry_value_new_* call would deadlock
 * the entire process. The sandboxed branch instead serialises the hang as
 * a JSON file under:
 *     $HOME/Library/Caches/PluginMakerPendingHangs/<uuid>.hangdump
 * (where $HOME has been rewritten by the appex sandbox to the host's per-
 * plugin writable container).
 *
 * On the NEXT launch — from `iplug::sentry::Init()` after `sentry_init`
 * succeeds — we scan that directory, build a `sentry_value_t` event per
 * file, and call `sentry_capture_event`. To stay crash-idempotent the
 * scanner uses a two-phase delete: rename `<uuid>.hangdump` to
 * `<uuid>.hangdump.uploading` BEFORE capture, then unlink after. A crash
 * mid-capture leaves `.uploading` files which are deleted (not re-uploaded)
 * on the next launch — they're already in sentry-native's local DB.
 *
 * Cap: process at most 5 files per launch and (to avoid blocking host
 * probes) the scan runs on a detached background thread spawned from
 * Init(). A stuck user could otherwise accumulate hundreds of dumps and
 * turn every plugin instantiation into a slow Sentry flood.
 *
 * Compile-time gating
 * -------------------
 *   When IPLUG_USE_SENTRY is NOT defined the entry point is an inline
 *   empty function, so callers do not pull in any sentry-native symbols.
 */

namespace iplug
{
namespace sentry
{
namespace pendingdump
{

#ifdef IPLUG_USE_SENTRY

  /** Scan the pending-hangs directory, upload up to 5 dumps, delete each
   *  consumed file. No-op when the directory is missing or empty.
   *
   *  May spawn a detached background thread for the actual I/O + capture
   *  work — callers should not assume completion on return. Returns
   *  immediately on the host thread to keep plugin construction snappy.
   *
   *  Precondition: `sentry_init` has already succeeded in this process.
   */
  void ScanAndUploadPendingDumps();

#else

  inline void ScanAndUploadPendingDumps() {}

#endif // IPLUG_USE_SENTRY

} // namespace pendingdump
} // namespace sentry
} // namespace iplug
