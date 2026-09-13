#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root/build-midi-queue"}
sdk_dir=${VST3_SDK_ROOT:-"$root/Dependencies/IPlug/VST3_SDK"}
targets=(midi-queue-test MidiQueueProbe-vst3)
if [[ "$(uname -s)" == "Darwin" ]]; then
  targets+=(MidiQueueProbe-au)
fi
cmake -S "$root/Tests/MidiQueueTest" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release -DIPLUG2_VST3_SDK_PATH="$sdk_dir" "$@"
cmake --build "$build_dir" --config Release --target "${targets[@]}" --parallel "${JOBS:-4}"
ctest --test-dir "$build_dir" --build-config Release --output-on-failure
