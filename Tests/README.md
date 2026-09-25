This is the location of various tests, which are currently just a few iPlug2 Projects

`bash Tests/run-au-sample-rate-tests.sh` builds the production AUv2 adapter on
macOS and registers an in-process AudioComponent with Apple's AudioToolbox.
It checks initialization before activation, repeated 44.1/48/96 kHz transitions,
active stream-format changes, invalid format/channel/bus rejection, block size,
explicit reset, host gain retention and stereo output for an instrument and an
effect. Tone generation, host input and render measurements remain in memory;
it uses no audio files, installed plug-ins, DAW or hardware device.
It also renders with a NULL `ioActionFlags` (what Logic on Intel sends), a NULL
`mData` buffer, and a missing timestamp or buffer list, while render admission
is silent, ready and failed, and host MIDI offsets past either end of the block,
which must be clamped into it. `IPLUG_TEST_ARCHS="arm64 x86_64"` builds and runs
both macOS slices; x86_64 runs under Rosetta on Apple Silicon.
Named host-input cases feed properties, parameters, callbacks and render calls
NULL pointers, short or oversized values and out-of-range indices (a short
GetProperty buffer, a NULL SetProperty value, an unknown clump or bus, extra
output buffers). Each case reports on its own; `IPLUG_AU_ONLY_CASE=<name>` runs
one in isolation. `Tests/run-midi-queue-tests.sh` covers the VST3 side: a
zero-frame flush without buffers, a NULL output channel, an extra host bus and
a NULL state stream.
Parameter names, labels and groups up to 127 bytes are stored whole and cut
only at a UTF-8 character boundary: the harness checks 34/70/100/200-byte
names in IParam, the long name an AU host reads, and the VST3 test checks the
decoded UTF-16 title of a 63-byte non-ASCII name.
Saved state and strings: an AU ClassInfo saved with an 82-byte preset name
reopens, NULL / non-dictionary / wrong-typed ClassInfo is refused, a short name
cut through a UTF-8 character is cut before it, enum labels up to 127 bytes and
preset names up to 255 bytes are stored whole and bounded, and VST3 units,
display strings (both directions) and preset names decode UTF-8; a NULL
display string or program-name buffer is refused rather than dereferenced.

For an optional external-host check, build a new bundle with
`bash Tests/run-au-sample-rate-tests.sh --bundle /absolute/new/IPlugAURateTest.component`.
Copy the complete bundle (not a symlink) into the user Audio Unit Components
folder, then refresh the macOS AudioComponentRegistrar before scanning it.
Do not replace an existing plug-in. Run
`python Tests/au-sample-rate-host.py /installed/path/IPlugAURateTest.component`
in an environment with Pedalboard, NumPy and Mido, and validate the same bundle
with `auval -v aumu Rate IpTs` and pluginval. Remove only this test bundle after
validation. The Python check keeps one host instance, reacquires parameter
handles after renders and checks pitch, gain and stereo with varying rates and
block sizes; all audio stays in memory. Pedalboard manages its own lifecycle,
so the direct AudioToolbox test remains the proof that no explicit reset is
required. PluginMaker workstation runs must use the mac-heavy command wrapper.

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

## VST3 MIDI controller queues

Run `VST3_SDK_ROOT=/path/to/VST3_SDK bash Tests/run-midi-queue-tests.sh` to test
all controller points, sample ordering with notes, and sustain/patch behavior
through both the adapter and the actual built VST3 binary. The runner also
builds a synthetic AU control on macOS. See [MidiQueueTest](MidiQueueTest/README.md)
for the headless host matrix and the Pedalboard VST3 queue limitation.
