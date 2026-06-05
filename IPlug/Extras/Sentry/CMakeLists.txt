# ============================================================================
# IPlug/Extras/Sentry — optional Sentry Native crash + perf telemetry.
#
# This sub-target is only built when the consuming plugin's CMake sets
# IPLUG_USE_SENTRY=ON. When the option is OFF (the default), no sentry-native
# is fetched, no extra code is compiled, no behaviour changes anywhere.
#
# When enabled, this:
#   - Fetches sentry-native via FetchContent (Crashpad backend on Windows,
#     Breakpad backend on macOS — chosen at sentry-native's CMake level).
#   - Builds the iPlug2::Sentry interface target that plugins link against.
#   - Forwards SENTRY_DSN / PLUGIN_ID / PLUGIN_VERSION compile definitions
#     to IPlugSentry.cpp via target_compile_definitions on the plugin.
#
# The plugin's CMake is responsible for passing -DSENTRY_DSN="..." etc.
# (the build worker injects these per-plugin — see backend/services/
# native-build* in the main alexh repo).
# ============================================================================

option(IPLUG_USE_SENTRY "Enable Sentry Native crash/perf telemetry for the plugin" OFF)

if(NOT IPLUG_USE_SENTRY)
  # Build IPlugSentry.cpp as a tiny static lib so the no-op stub of
  # iplug::sentry::Init (the #else branch inside IPlugSentry.cpp) is
  # actually compiled into an object file. IPlugAPIBase.cpp calls Init()
  # unconditionally; without this static lib the symbol is undefined
  # and every plugin fails to link.
  add_library(iPlug2_Sentry STATIC
    ${CMAKE_CURRENT_LIST_DIR}/IPlugSentry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/IPlugSentry.h
  )
  target_include_directories(iPlug2_Sentry
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}
  )
  set_target_properties(iPlug2_Sentry PROPERTIES FOLDER "iPlug2/Extras")
  add_library(iPlug2::Sentry ALIAS iPlug2_Sentry)
  return()
endif()

include(FetchContent)

# Pinned sentry-native release. Bump deliberately + re-test cross-platform.
# 0.7.x line is current-stable as of May 2026 and supports Crashpad + Breakpad
# backends out of the box.
set(SENTRY_NATIVE_VERSION "0.7.16")

# Backend choice:
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

add_library(iPlug2_Sentry STATIC
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentry.cpp
  ${CMAKE_CURRENT_LIST_DIR}/IPlugSentry.h
)

target_include_directories(iPlug2_Sentry
  PUBLIC ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(iPlug2_Sentry
  PUBLIC IPLUG_USE_SENTRY
)

# Forward per-plugin metadata from CMake variables into preprocessor defines
# so IPlugSentry.cpp's compile-time SENTRY_DSN guard + tag macros pick them up.
# The build worker passes these via `-DSENTRY_DSN=... -DPLUGIN_ID=... -DPLUGIN_VERSION=...`
# on the cmake command line; without this forwarding they remain CMake-only
# variables and never reach the C++ side, so sentry_init silently no-ops even
# with IPLUG_USE_SENTRY=ON.
if(DEFINED SENTRY_DSN AND NOT SENTRY_DSN STREQUAL "")
  target_compile_definitions(iPlug2_Sentry PRIVATE SENTRY_DSN="${SENTRY_DSN}")
endif()
if(DEFINED PLUGIN_ID AND NOT PLUGIN_ID STREQUAL "")
  target_compile_definitions(iPlug2_Sentry PUBLIC PLUGIN_ID="${PLUGIN_ID}")
endif()
if(DEFINED PLUGIN_VERSION AND NOT PLUGIN_VERSION STREQUAL "")
  target_compile_definitions(iPlug2_Sentry PUBLIC PLUGIN_VERSION="${PLUGIN_VERSION}")
endif()
# A-8 tag scheme — see IPlugSentry.cpp's `sentry_set_tag("brandSlug", ...)`
# and `("generatedByPrompt", ...)` blocks. The build worker bakes both via
# brand.json (BRAND_SLUG) and the backend (GENERATED_BY_PROMPT). Skipping
# the define when the var is unset/empty means the corresponding tag never
# gets set, instead of getting "(null)" / "" — keeps Sentry's Discover
# queries clean.
#
# Length caps are defense-in-depth: the alexh worker's safeCmakeDefineValue
# already gates allowed chars + length, but a runaway value would bloat
# every shipped binary (each tag is baked as a string literal) AND every
# Sentry event payload. Hard-fail at CMake time if a caller bypasses the
# worker validation — better than silently shipping a 10 KiB tag.
if(DEFINED BRAND_SLUG AND NOT BRAND_SLUG STREQUAL "")
  string(LENGTH "${BRAND_SLUG}" _brand_slug_len)
  if(_brand_slug_len GREATER 64)
    message(FATAL_ERROR "BRAND_SLUG too long (${_brand_slug_len} chars; max 64). Validate at the build worker, not here.")
  endif()
  target_compile_definitions(iPlug2_Sentry PUBLIC BRAND_SLUG="${BRAND_SLUG}")
endif()
if(DEFINED GENERATED_BY_PROMPT AND NOT GENERATED_BY_PROMPT STREQUAL "")
  string(LENGTH "${GENERATED_BY_PROMPT}" _gen_by_prompt_len)
  if(_gen_by_prompt_len GREATER 64)
    message(FATAL_ERROR "GENERATED_BY_PROMPT too long (${_gen_by_prompt_len} chars; max 64). The backend bakes a 12-char sha256 prefix — anything longer is a misconfiguration.")
  endif()
  target_compile_definitions(iPlug2_Sentry PUBLIC GENERATED_BY_PROMPT="${GENERATED_BY_PROMPT}")
endif()

target_link_libraries(iPlug2_Sentry
  PUBLIC sentry
)

if(WIN32)
  # Crashpad ships a helper executable that must live next to the plugin
  # DLL at runtime. Expose the path so the plugin's deploy step can copy it.
  set_property(TARGET iPlug2_Sentry PROPERTY IPLUG_SENTRY_HELPER
    $<TARGET_FILE:crashpad::handler>)
endif()

set_target_properties(iPlug2_Sentry PROPERTIES FOLDER "iPlug2/Extras")

add_library(iPlug2::Sentry ALIAS iPlug2_Sentry)
