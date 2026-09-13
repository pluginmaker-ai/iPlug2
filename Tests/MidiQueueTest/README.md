# VST3 MIDI queue regression (PLU-895)

`IPlugVST3ProcessorBase` merges MIDI proxy queue points with input events before
calling `ProcessMidiMsg`. Ordinary parameters and bypass keep their last-point
behavior. Each sorted host queue contributes one cursor to a fixed-size heap:
space is bounded by the advertised MIDI controller namespace, regardless of
how many points arrive. Processing costs O(queues + points × log(queues)), plus
the event-list traversal. There is no heap allocation in the merge.

For equal sample offsets, controllers precede input-list events, as in the old
adapter. Ties between controller queues follow the host's parameter-queue order;
ties within a queue or event list retain that list's order. VST3's separate
[parameter queues](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IParamValueQueue.html)
and [event list](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IEventList.html)
do not carry an original cross-list serial order. The merge consumes their
sample-ordered streams; it does not invent additional ordering information.
Editor MIDI retains the existing delivery after host MIDI. The bounded UI mirror
queue can fill, but its capacity never limits delivery to `ProcessMidiMsg`.

## Build and run

Requires CMake, a C++17 toolchain, and the VST3 SDK used by the native build.

```bash
VST3_SDK_ROOT=/absolute/path/to/VST3_SDK bash Tests/run-midi-queue-tests.sh
```

Or use CMake directly (also works from a Windows developer shell):

```text
cmake -S Tests/MidiQueueTest -B build-midi-queue -DCMAKE_BUILD_TYPE=Release -DIPLUG2_VST3_SDK_PATH=/absolute/path/to/VST3_SDK
cmake --build build-midi-queue --config Release --target midi-queue-test MidiQueueProbe-vst3
ctest --test-dir build-midi-queue -C Release --output-on-failure
```

On macOS the shell runner also builds `MidiQueueProbe-au`. It does not install
anything or open a DAW. Outputs are under `build-midi-queue/out/`.

The two CTest cases execute the same assertions against the linked adapter and
against the actual VST3 bundle loaded through its SDK factory. The latter calls
`IAudioProcessor::process` through the loaded instance's interface, not through
the host's linked copy. A fixture-only exported accessor exposes captured MIDI
for assertions; it is absent from production plugins. Cases cover:

- Every CC64/CC22 point, interleaved queues, all 16 MIDI channels and equal offsets.
- Note-on/off ordering, pitch bend, channel/poly pressure and SysEx timing.
- Empty and single-point queues, failed host getters, invalid IDs, zero-frame
  parameter flushes, ordinary parameter offsets, and bypass activation/release.
- The full advertised MIDI namespace and 20,000 points in one block, even after
  the UI mirror queue fills; a C++ allocation counter surrounds `process()`.
- Sustain and patch selection at 32, 512, 1024 and 8,192 frames in both real-time
  and offline modes, compared sample by sample with an independent oracle.

`MidiQueueProbe` is a synchronous, asset-free test instrument. Its output level
encodes notes: CC64 holds released notes; CC22 chooses the level latched by the
next note. It needs no license, sample library, audio files, device or warmup.
The musical matrix runs both CC endpoints and float-normalized CC64=63/64
plus CC22=7/4, so the cumulative pin checks PLU-894 reconstruction together
with PLU-895 scheduling. The note level preserves all 128 patch values.

## Headless host and AU control

With the Python environment from alexh's `/host-test-plugin` (pedalboard + numpy):

```text
python Tests/MidiQueueTest/host_midi_test.py /absolute/path/MidiQueueProbe.vst3 --scenario notes
python Tests/MidiQueueTest/host_midi_test.py /absolute/path/MidiQueueProbe.component
```

AU discovery requires installing the exact task-owned bundle under the user's
Components directory with a unique name. Remove only that bundle after testing.
Every scenario runs in its own subprocess and checks both channels, an audible
reference window, every output sample and non-finite values. All audio remains
in memory. For parameter smoke checks, run alexh's
`instrument_parameter_test.py <bundle> --require-param gain`. Instruments do not
have an effect-style bypass-null contract; this fixture's bypass behavior is
covered directly in the adapter test.

**Pedalboard 0.9.24 is not a valid VST3 CC scheduling oracle.** Its
[`ParamValueQueue`](https://github.com/spotify/pedalboard/blob/v0.9.24/pedalboard/juce_overrides/juce_PatchedVST3PluginFormat.cpp#L1910-L1968)
stores only the latest value and returns offset zero. The full CC matrix against
that host correctly fails even for a repaired adapter: the lost points never
reach the plugin. Use the native SDK host for VST3 CC proof, Pedalboard for the
VST3 note-only smoke, and the full Pedalboard AU matrix as the control reference.
Do not weaken the CC oracle or call the VST3 Pedalboard failure a passing test.

Run pluginval against both exact bundles with `--strictness-level 10
--skip-gui-tests`. Run `auval -v aumu Mq95 IpTs` for the installed AU. These checks
complement the musical assertions; validators alone cannot prove event fidelity.

## Integration boundary

This fork PR targets `alexh.bob-feat/plu-861-songkeys-native-audit` at deployed
base `5dc27d51119fa80dfb070263f35761d2f34b84fd`. It does not change alexh's pin or
provenance; PLU-896 owns the cumulative update. Preserve PLU-894's integer
conversion changes when integrating its edits to the same conversion block.

Song Keys library playback with ordinary CC values after PLU-894, Windows native
validation, and the customer candidate remain integration checks. DAW GUI,
project reopen, bounce and hardware acceptance are **Deferred to PLU-861 — final
integrated DAW testing**. Synthetic fixture results are not a customer release.
