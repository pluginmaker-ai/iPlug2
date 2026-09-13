This is the location of various tests, which are currently just a few iPlug2 Projects

- **[IGraphicsTest](https://iplug2.github.io/NANOVG/IGraphicsTest/)** : An IPlug project that includes many controls to test different functionality 
  of IGraphics, with different drawing and platform backends.
  
- **[IGraphicsStressTest](https://iplug2.github.io/NANOVG/IGraphicsStressTest/)** : An IPlug project to test drawing lots of things

- **[MetaParamTest]((https://iplug2.github.io/NANOVG/MetaParamTest/))** : An IPlug project to test parameters that affect other parameters, a.k.a. Meta Parameters

## Native WebView corner resizing

Run `bash Tests/run-webview-resize-tests.sh` from the repository root. The portable
C++ test checks minimum and maximum dimensions, custom design sizes, aspect
ratios, integer rounding, and repeated negotiation. On macOS it also compiles
the actual native resize handle and verifies its fixed 24-point hit area and
the bounded frames delivered to AppKit observers during shrink/grow cycles.
It exercises mouse events while the host moves its window and loads legacy,
AU, and VST3 handle classes together to detect Objective-C name collisions.
These checks use no audio files. Actual AU/VST3 editor behavior still needs
verification inside DAWs, including Logic's window chrome.

## MIDI integer reconstruction

The standalone regression compiles the production `IPlugMidi.h` and needs only
CMake and a C++17 compiler, without plugin SDKs or audio files:

```sh
cmake -S Tests/Midi -B build/midi-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/midi-tests --config Release
ctest --test-dir build/midi-tests -C Release --output-on-failure -V
```

Repeat with `Debug` and a separate build directory to check both configurations.
With GCC or Clang, add `-DIPLUG_MIDI_SANITIZERS=ON` at configure time for
undefined-behavior and floating-to-integer overflow checks.

It exhaustively checks float/double division and reciprocal multiplication for
all 128 values, every controller and MIDI channel, and several sample offsets.
This covers CC22 slots 0–7 and both sides of CC64's sustain threshold, along with
nearest rounding, saturation, NaN/infinities, existing integer message builders
and the unchanged 14-bit pitch-wheel convention. VST3 note-on velocity and both
aftertouch inputs use the same normalized-to-7-bit helper: the old casts also
lost values there (including note velocities from VST3's reciprocal output path).

On macOS, also compile and execute the real `IPlugVST3ProcessorBase` by supplying
an existing VST3 SDK (including `pluginterfaces`, `base` and `public.sdk`) at
configure time:

```sh
cmake -S Tests/Midi -B build/vst3-midi-tests -DCMAKE_BUILD_TYPE=Release \
  -DIPLUG_VST3_SDK_DIR=/absolute/path/to/VST3_SDK
cmake --build build/vst3-midi-tests --config Release
ctest --test-dir build/vst3-midi-tests -C Release --output-on-failure -V
```

The processor test passes synthetic single-point parameter queues and event
lists through the production adapter and checks both the plugin MIDI callback
and the editor-forwarding queue. It covers all integer values and channels for
CC22, CC64, channel aftertouch, note velocity and poly aftertouch. It does not
test multi-point queue traversal or use audio files.
This optional processor target is macOS-only: it uses the framework's native
timer/platform sources. Omit `IPLUG_VST3_SDK_DIR` on Linux and Windows to run
the portable production-header regression, which CI checks on all three systems.

These are numeric/framework checks. For Song Keys, actual built VST3 slot
selection, sustain/audio, validators and final integrated DAW testing remain
part of PLU-861 after PLU-896 imports the reviewed fork commit and updates the
alexh pin/provenance pair. This regression does not update that pin or rebuild
customer artifacts.
