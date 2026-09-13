#!/usr/bin/env python3
"""Optional Pedalboard check of the AU rate fixture; all audio stays in memory."""

import json
import subprocess
import sys


def check(bundle):
    import numpy as np
    from mido import Message
    from pedalboard import load_plugin

    plugin = load_plugin(bundle)
    plugin.parameters["gain"].raw_value = 0.37
    for rate, frames in [(48000, 64), (44100, 512), (96000, 1024), (48000, 128), (44100, 256)]:
        audio = plugin(
            [Message("note_on", note=69, velocity=100, time=0)],
            duration=0.25, sample_rate=rate, buffer_size=frames,
            num_channels=2, reset=False,
        )
        assert audio.shape == (2, round(rate * 0.25)), audio.shape
        assert np.isfinite(audio).all(), "non-finite output"
        assert np.max(np.abs(audio[1] - 0.5 * audio[0])) < 1e-6, "stereo changed"
        crossings = np.flatnonzero((audio[0, :-1] < 0) & (audio[0, 1:] >= 0))
        assert len(crossings) > 50, "tone is silent"
        positions = crossings - audio[0, crossings] / (audio[0, crossings + 1] - audio[0, crossings])
        pitch = rate * (len(positions) - 1) / (positions[-1] - positions[0])
        peak = float(np.max(np.abs(audio)))
        assert abs(pitch - 440) < 0.05, pitch
        assert abs(peak - 0.37) < 0.001, peak
        # Reacquire the handle after rendering; hosts may rebuild parameters.
        assert abs(plugin.parameters["gain"].raw_value - 0.37) < 0.001, "parameter lost"
        print(json.dumps({"rate": rate, "block": frames, "pitch": float(pitch), "peak": peak, "nonFinite": 0}))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[2] == "--child":
        check(sys.argv[1])
    elif len(sys.argv) == 2:
        result = subprocess.run([sys.executable, __file__, sys.argv[1], "--child"], timeout=60)
        print("RESULT:", "pass" if result.returncode == 0 else f"fail (child exit {result.returncode})")
        sys.exit(0 if result.returncode == 0 else 1)
    else:
        sys.exit("usage: au-sample-rate-host.py /installed/path/IPlugAURateTest.component")
