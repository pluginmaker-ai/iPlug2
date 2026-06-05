# ============================================================================
# IPlug/Extras/Sentry — optional Sentry Native crash + perf telemetry.
#
# This sub-target is only built when the consuming plugin's CMake sets
# IPLUG_USE_SENTRY=ON. When the option is OFF (the default), the iPlug2::Sentry
# target is an empty INTERFACE library — no sentry-native is fetched, no extra
# code is compiled, no behaviour changes anywhere. The header-side #else
# branches provide inline no-op definitions of every entry point so callers
# never need to ifdef their call sites.
#
# When enabled, this:
#   - Fetches sentry-native via FetchContent (Crashpad backend on Windows,
#     Breakpad backend on macOS — chosen at sentry-native's CMake level).
#   - Builds the iPlug2::Sentry STATIC target with the watchdog + pending-dump
#     TUs alongside IPlugSentry.cpp.
#   - Forwards SENTRY_DSN / PLUGIN_ID / PLUGIN_VERSION / BRAND_SLUG /
#     GENERATED_BY_PROMPT compile definitions as PRIVATE so they reach the
#     Sentry TUs WITHOUT leaking into every plugin TU (avoids macro
#     redefinition warnings against common names like PLUGIN_ID).
#
# The plugin's CMake is responsible for passing -DSENTRY_DSN="..." etc.
# (the build worker injects these per-plugin — see backend/services/
# native-build* in the main alexh repo).
#
# A-5 surface (audio-thread watchdog) adds three new TUs:
#   - IPlugSentryWatchdog.cpp           cross-platform watchdog thread
#   - IPlugSentryWatchdog_mac.mm        macOS sandbox detection + capture
#   - IPlugSentryPendingDump.cpp        cross-platform dump scanner
# Note: A-5 watchdog ships hand-built sentry_value_t events via
# sentry_capture_event and does NOT depend on the configured Crashpad/
# Breakpad backend. Backend choice only affects native-crash captures
# elsewhere in IPlugSentry.cpp.
# ============================================================================

option(IPLUG_USE_SENTRY "Enable Sentry Native crash/perf telemetry for the plugin" OFF)

set(_iplug_sentry_common_sources
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentry.cpp
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentry.h
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentryWatchdog.cpp
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentryWatchdog.h
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentryPendingDump.cpp
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentryPendingDump.h
)

if(APPLE)
  list(APPEND _iplug_sentry_common_sources
    ${CMAKE_CURRENT_LIST_DIR}/IPlugSentryWatchdog_mac.mm
  )
endif()

if(NOT IPLUG_USE_SENTRY)
  # OFF build: iPlug2::Sentry is a pure INTERFACE library. The headers'
  # #else branches provide inline no-op definitions, so IPlugProcessor.cpp
  # and IPlugAPIBase.cpp can include the headers and call into the API
  # without linking any symbol — nothing to compile, nothing to link.
  add_library(iPlug2_Sentry INTERFACE)
  target_include_directories(iPlug2_Sentry
    INTERFACE ${CMAKE_CURRENT_LIST_DIR}
  )
  add_library(iPlug2::Sentry ALIAS iPlug2_Sentry)
  return()
endif()

include(FetchContent)

set(SENTRY_NATIVE_VERSION "0.7.16")

# Backend choice (native crash capture only, NOT used by the A-5 watchdog):
#   Windows -> Crashpad (out-of-process helper)
#   macOS   -> Breakpad (in-process; Crashpad's helper can't spawn under the
#              sandboxed AU host process used by Logic Pro / GarageBand)
if(WIN32)
  set(SENTRY_BACKEND "crashpad" CACHE STRING "Sentry backend" FORCE)
elseif(APPLE)
  set(SENTRY_BACKEND "breakpad" CACHE STRING "Sentry backend" FORCE)
endif()

set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "Link sentry-native statically" FORCE)
set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  sentry_native
  GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
  GIT_TAG ${SENTRY_NATIVE_VERSION}
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sentry_native)

add_library(iPlug2_Sentry STATIC ${_iplug_sentry_common_sources})

target_include_directories(iPlug2_Sentry
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(iPlug2_Sentry
  PUBLIC IPLUG_USE_SENTRY
)

# Forward per-plugin metadata. Visibility:
#   - SENTRY_DSN: PRIVATE. Only IPlugSentry.cpp reads it.
#   - PLUGIN_ID / PLUGIN_VERSION: PUBLIC. IPlugAPIBase.cpp (NOT a Sentry-target
#     TU — lives in iPlug2 core) calls `iplug::sentry::Init(PLUGIN_ID,
#     PLUGIN_VERSION)` at line 46. Plugin targets that link iPlug2::Sentry
#     need these macros visible at compile time on every TU. PRIVATE would
#     silently regress the tag values to the IPlugAPIBase.cpp fallback ("")
#     in production. IPlugSentryWatchdog_mac.mm also references them — it's
#     PRIVATE-reachable inside the target, so PUBLIC covers both consumers.
#   - BRAND_SLUG / GENERATED_BY_PROMPT: PRIVATE (see below). Only the Sentry
#     target's TUs read them, so we don't leak them into user code.
if(DEFINED SENTRY_DSN AND NOT SENTRY_DSN STREQUAL "")
  target_compile_definitions(iPlug2_Sentry PRIVATE SENTRY_DSN="${SENTRY_DSN}")
endif()
if(DEFINED PLUGIN_ID AND NOT PLUGIN_ID STREQUAL "")
  target_compile_definitions(iPlug2_Sentry PUBLIC PLUGIN_ID="${PLUGIN_ID}")
endif()
if(DEFINED PLUGIN_VERSION AND NOT PLUGIN_VERSION STREQUAL "")
  target_compile_definitions(iPlug2_Sentry PUBLIC PLUGIN_VERSION="${PLUGIN_VERSION}")
endif()
# A-8 tag scheme — see IPlugSentry.cpp's brandSlug / generatedByPrompt blocks.
# Length caps are defense-in-depth: the alexh worker's safeCmakeDefineValue
# already validates allowed chars + length, but a runaway value would bloat
# every shipped binary (each tag is baked as a string literal) AND every
# Sentry event payload. Hard-fail at CMake time if a caller bypasses the
# worker validation.
if(DEFINED BRAND_SLUG AND NOT BRAND_SLUG STREQUAL "")
  string(LENGTH "${BRAND_SLUG}" _brand_slug_len)
  if(_brand_slug_len GREATER 64)
    message(FATAL_ERROR "BRAND_SLUG too long (${_brand_slug_len} chars; max 64). Validate at the build worker, not here.")
  endif()
  target_compile_definitions(iPlug2_Sentry PRIVATE BRAND_SLUG="${BRAND_SLUG}")
endif()
if(DEFINED GENERATED_BY_PROMPT AND NOT GENERATED_BY_PROMPT STREQUAL "")
  string(LENGTH "${GENERATED_BY_PROMPT}" _gen_by_prompt_len)
  if(_gen_by_prompt_len GREATER 64)
    message(FATAL_ERROR "GENERATED_BY_PROMPT too long (${_gen_by_prompt_len} chars; max 64). The backend bakes a 12-char sha256 prefix — anything longer is a misconfiguration.")
  endif()
  target_compile_definitions(iPlug2_Sentry PRIVATE GENERATED_BY_PROMPT="${GENERATED_BY_PROMPT}")
endif()

# Optional debug knob for QA: deterministic hang simulation.
if(IPLUG_SENTRY_WATCHDOG_DEBUG)
  target_compile_definitions(iPlug2_Sentry PUBLIC IPLUG_SENTRY_WATCHDOG_DEBUG)
endif()

target_link_libraries(iPlug2_Sentry
  PUBLIC sentry
)

if(APPLE)
  # The macOS watchdog .mm uses Foundation for NSBundle / NSFileManager.
  # mach + libSystem are always linked; Foundation needs to be listed.
  # CMake auto-detects .mm as Objective-C++ from the extension — no
  # explicit LANGUAGE property needed.
  target_link_libraries(iPlug2_Sentry PUBLIC "-framework Foundation")
endif()

if(WIN32)
  set_property(TARGET iPlug2_Sentry PROPERTY IPLUG_SENTRY_HELPER
    $<TARGET_FILE:crashpad::handler>)
endif()

set_target_properties(iPlug2_Sentry PROPERTIES FOLDER "iPlug2/Extras")

add_library(iPlug2::Sentry ALIAS iPlug2_Sentry)
