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

## VST3 MIDI controller queues

Run `VST3_SDK_ROOT=/path/to/VST3_SDK bash Tests/run-midi-queue-tests.sh` to test
all controller points, sample ordering with notes, and sustain/patch behavior
through both the adapter and the actual built VST3 binary. The runner also
builds a synthetic AU control on macOS. See [MidiQueueTest](MidiQueueTest/README.md)
for the headless host matrix and the Pedalboard VST3 queue limitation.
