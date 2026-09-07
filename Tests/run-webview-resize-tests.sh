#!/usr/bin/env bash
set -euo pipefail

test_dir=$(cd "$(dirname "$0")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/iplug-webview-resize.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
compiler=${CXX:-c++}

"$compiler" -std=c++17 -Wall -Wextra -Werror \
  "$test_dir/IPlugWebViewCornerResizeTest.cpp" -o "$build_dir/corner-resize"
"$build_dir/corner-resize"

if [[ "$(uname -s)" == "Darwin" ]]; then
  "$compiler" -std=c++17 -fno-objc-arc -Wall -Wextra -Werror -Wno-unused-parameter \
    -framework AppKit "$test_dir/IPlugWebViewResizeHandleTest.mm" -o "$build_dir/resize-handle"
  "$build_dir/resize-handle"
fi
