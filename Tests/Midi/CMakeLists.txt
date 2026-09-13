cmake_minimum_required(VERSION 3.16)
project(IPlugMidiTests LANGUAGES CXX)

enable_testing()

add_executable(iplug-midi-test IPlugMidiTest.cpp)
target_compile_features(iplug-midi-test PRIVATE cxx_std_17)
# The production header includes existing framework/WDL utilities. Keep their
# unrelated warnings out of this standalone test's strict diagnostics.
target_include_directories(iplug-midi-test SYSTEM PRIVATE ../../IPlug ../../WDL)

if(MSVC)
  target_compile_options(iplug-midi-test PRIVATE /W4 /WX)
else()
  target_compile_options(iplug-midi-test PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME midi-conversion COMMAND iplug-midi-test)
set_tests_properties(midi-conversion PROPERTIES TIMEOUT 60)

set(IPLUG_VST3_SDK_DIR "" CACHE PATH "Existing VST3 SDK for the macOS processor regression")
if(IPLUG_VST3_SDK_DIR)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The optional VST3 processor harness is macOS only. Omit IPLUG_VST3_SDK_DIR for the portable header regression.")
  endif()
  if(NOT EXISTS "${IPLUG_VST3_SDK_DIR}/pluginterfaces/vst/ivstparameterchanges.h")
    message(FATAL_ERROR "IPLUG_VST3_SDK_DIR must contain the VST3 pluginterfaces headers")
  endif()
  add_executable(iplug-vst3-midi-test
    IPlugVST3MidiTest.cpp
    ../../IPlug/VST3/IPlugVST3_ProcessorBase.cpp
    ../../IPlug/IPlugAPIBase.cpp
    ../../IPlug/IPlugPluginBase.cpp
    ../../IPlug/IPlugProcessor.cpp
    ../../IPlug/IPlugParameter.cpp
    ../../IPlug/IPlugTimer.cpp
    ../../IPlug/Extras/Sentry/IPlugSentry.cpp)
  target_compile_features(iplug-vst3-midi-test PRIVATE cxx_std_17)
  target_compile_definitions(iplug-vst3-midi-test PRIVATE NO_IGRAPHICS VST3_API $<$<CONFIG:Debug>:_DEBUG>)
  target_include_directories(iplug-vst3-midi-test SYSTEM PRIVATE
    ../../IPlug ../../IPlug/VST3 ../../IPlug/Extras ../../WDL "${IPLUG_VST3_SDK_DIR}")
  if(APPLE)
    target_link_libraries(iplug-vst3-midi-test PRIVATE "-framework CoreFoundation")
  endif()
  add_test(NAME vst3-midi-conversion COMMAND iplug-vst3-midi-test)
  set_tests_properties(vst3-midi-conversion PROPERTIES TIMEOUT 60)
endif()

option(IPLUG_MIDI_SANITIZERS "Enable undefined-behavior and floating-to-integer overflow checks" OFF)
if(IPLUG_MIDI_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "IPLUG_MIDI_SANITIZERS requires GCC or Clang")
  endif()
  set(sanitizers -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all)
  foreach(test_target IN ITEMS iplug-midi-test iplug-vst3-midi-test)
    if(TARGET ${test_target})
      target_compile_options(${test_target} PRIVATE ${sanitizers} -fno-omit-frame-pointer)
      target_link_options(${test_target} PRIVATE ${sanitizers})
      if(test_target STREQUAL "iplug-vst3-midi-test")
        # The harness does not construct SDK audio buses or link their runtime.
        # Avoid RTTI references from unused audio-bus methods in the same .cpp;
        # retain the integer/float/other UB checks for the exercised MIDI path.
        target_compile_options(${test_target} PRIVATE -fno-sanitize=vptr)
      endif()
    endif()
  endforeach()
endif()
