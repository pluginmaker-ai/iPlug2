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
  # Stub target so plugin CMake can unconditionally link iPlug2::Sentry
  # without `if(IPLUG_USE_SENTRY)` guards everywhere.
  add_library(iPlug2_Sentry INTERFACE)
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
