#!/usr/bin/env python3
"""MIDI-driven /host-test-plugin variant for MidiQueueProbe. No audio file I/O.

Requires pedalboard and numpy. Every render runs in a separate process. Run
against the built VST3 and the same task-owned AU installed for AU discovery.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

RATE = 48000
FRAMES = 16384
SCENARIOS = ("notes", "sustain", "short-sustain", "patch", "channels", "ties")


def scenario(name):
    # (sample, status, data1, data2), expected (start, stop, amplitude)
    if name == "notes":
        return [(96, 0x90, 60, 127), (6768, 0x80, 60, 0)], [(96, 6768, 0.02)]
    if name == "sustain":
        return [(48, 0xB0, 64, 127), (96, 0x90, 60, 127),
                (192, 0x80, 60, 0), (6768, 0xB0, 64, 0)], [(96, 6768, 0.02)]
    if name == "short-sustain":
        return [(4, 0xB0, 64, 127), (8, 0x90, 60, 127),
                (16, 0x80, 60, 0), (160, 0xB0, 64, 0)], [(8, 160, 0.02)]
    if name == "patch":
        return [(480, 0xB0, 22, 127), (960, 0x90, 60, 127),
                (5760, 0xB0, 22, 0), (7200, 0x80, 60, 0)], [(960, 7200, 0.04)]
    if name == "channels":
        return [(48, 0xB1, 64, 127), (96, 0x91, 60, 127),
                (120, 0xBF, 22, 127), (144, 0x9F, 64, 127),
                (192, 0x81, 60, 0), (240, 0x8F, 64, 0),
                (480, 0xB1, 64, 0)], [(96, 480, 0.02), (144, 240, 0.04)]
    if name == "ties":
        return [(48, 0xB0, 22, 127), (48, 0xB0, 64, 127),
                (48, 0x90, 60, 127), (96, 0x80, 60, 0),
                (144, 0xB0, 64, 0), (144, 0x90, 60, 127),
                (144, 0x80, 60, 0)], [(48, 144, 0.04)]
    raise ValueError(name)


def child(args):
    import numpy as np
    from pedalboard import load_plugin

    plug = load_plugin(args.plugins[0])
    events, windows = scenario(args.scenario)
    messages = [(bytes([status, data1, data2]), offset / RATE)
                for offset, status, data1, data2 in events]
    # The fixture has synchronous, asset-free readiness. A nonzero reference
    # window proves it actually ran; a silent instance cannot pass this test.
    audio = np.asarray(plug(messages, duration=FRAMES / RATE, sample_rate=RATE,
                            num_channels=2, buffer_size=args.block))
    expected = np.zeros((2, FRAMES), dtype=np.float32)
    for start, stop, level in windows:
        expected[:, start:stop] += level
    if audio.shape != expected.shape:
        raise AssertionError(f"shape {audio.shape}, expected {expected.shape}")
    non_finite = int(np.count_nonzero(~np.isfinite(audio)))
    error = float(np.max(np.abs(audio - expected)))
    if non_finite or error > 1e-6:
        mismatch = np.flatnonzero(np.abs(audio[0] - expected[0]) > 1e-6)
        raise AssertionError(f"nonFinite={non_finite}, error={error}, first mismatches={mismatch[:12].tolist()}")
    print(json.dumps({"scenario": args.scenario, "block": args.block,
                      "maxError": error, "nonFinite": non_finite,
                      "peak": float(np.max(np.abs(audio)))}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugins", nargs="+")
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--scenario", choices=SCENARIOS)
    parser.add_argument("--block", type=int)
    args = parser.parse_args()
    if args.child:
        child(args)
        return
    count = 0
    for plugin in args.plugins:
        for name in (args.scenario,) if args.scenario else SCENARIOS:
            for block in (32, 512, 1024, 8192):
                result = subprocess.run(
                    [sys.executable, str(Path(__file__).resolve()), plugin,
                     "--child", "--scenario", name, "--block", str(block)],
                    capture_output=True, text=True, timeout=60, check=False)
                if result.returncode:
                    raise RuntimeError(f"{plugin} {name}/{block} failed ({result.returncode}):\n"
                                       f"{result.stdout}\n{result.stderr}")
                print(f"PASS: {Path(plugin).suffix} {result.stdout.strip()}", flush=True)
                count += 1
    print(f"RESULT: pass — {count} native renders match the sample-by-sample oracle")


if __name__ == "__main__":
    main()
