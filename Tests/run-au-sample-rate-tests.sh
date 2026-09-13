#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
  echo 'AU lifecycle tests require macOS and the Apple AudioToolbox framework.' >&2
  exit 2
fi

test_dir=$(cd "$(dirname "$0")" && pwd)
iplug_dir=$(cd "$test_dir/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/iplug-au-rate.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
compiler=${CXX:-clang++}
output="$build_dir/au-rate-test"
bundle_args=()
bundle_dir=
if [[ "$#" == 2 && "$1" == --bundle ]]; then
  bundle_dir=$2
  [[ "$bundle_dir" = /* && ! -e "$bundle_dir" ]] || {
    echo 'Bundle output must be an absolute, new directory.' >&2
    exit 2
  }
  mkdir -p "$bundle_dir/Contents/MacOS"
  output="$bundle_dir/Contents/MacOS/IPlugAURateTest"
  bundle_args=(-DIPLUG_AU_RATE_TEST_BUNDLE -bundle)
elif [[ "$#" != 0 ]]; then
  echo 'usage: run-au-sample-rate-tests.sh [--bundle /new/path/IPlugAURateTest.component]' >&2
  exit 2
fi

clang -Wno-deprecated-declarations -c "$iplug_dir/IPlug/AUv2/dfx-au-utilities.c" \
  -o "$build_dir/dfx-au-utilities.o"

"$compiler" -std=c++17 -O1 -x objective-c++ -fno-objc-arc \
  "${bundle_args[@]}" \
  -Wno-deprecated-declarations -Wno-deprecated-register -Wno-#warnings \
  -DAU_API -DAU_NO_COMPONENT_ENTRY -DIPLUG_DSP=1 -DIPLUG_EDITOR=0 -DNO_IGRAPHICS \
  -I"$iplug_dir/IPlug" -I"$iplug_dir/IPlug/AUv2" -I"$iplug_dir/WDL" \
  -I"$iplug_dir/IPlug/Extras" \
  "$test_dir/IPlugAUSampleRateTest.mm" \
  "$iplug_dir/IPlug/AUv2/IPlugAU.cpp" \
  "$iplug_dir/IPlug/IPlugAPIBase.cpp" \
  "$iplug_dir/IPlug/IPlugPluginBase.cpp" \
  "$iplug_dir/IPlug/IPlugProcessor.cpp" \
  "$iplug_dir/IPlug/IPlugParameter.cpp" \
  "$iplug_dir/IPlug/IPlugTimer.cpp" \
  "$iplug_dir/IPlug/IPlugPaths.cpp" \
  "$iplug_dir/IPlug/IPlugPaths.mm" \
  -x none "$build_dir/dfx-au-utilities.o" \
  -framework AudioUnit -framework AudioToolbox -framework CoreAudio \
  -framework CoreMIDI -framework Cocoa -o "$output"
if [[ -n "$bundle_dir" ]]; then
  python3 - "$bundle_dir" <<'PY'
import pathlib, plistlib, sys
contents = pathlib.Path(sys.argv[1]) / 'Contents'
info = {
    'CFBundleIdentifier': 'org.iplug2.rate-test',
    'CFBundleExecutable': 'IPlugAURateTest',
    'CFBundleName': 'AU Rate Test',
    'CFBundlePackageType': 'BNDL',
    'CFBundleVersion': '1.0.0',
    'CFBundleShortVersionString': '1.0.0',
    'AudioComponents': [{
        'name': 'iPlug2 Tests: AU Rate Test', 'description': 'In-memory sample-rate regression fixture',
        'manufacturer': 'IpTs', 'type': 'aumu', 'subtype': 'Rate', 'version': 65536,
        'factoryFunction': 'IPlugAURateTestFactory', 'sandboxSafe': True,
    }],
}
(contents / 'Info.plist').write_bytes(plistlib.dumps(info))
(contents / 'PkgInfo').write_bytes(b'BNDL????')
PY
  codesign --force --sign - "$bundle_dir"
  echo "Built AU fixture: $bundle_dir"
else
  "$output"
fi
