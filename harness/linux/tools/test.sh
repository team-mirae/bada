#!/bin/sh
# Builds and tests the Linux harness, on a Linux machine with Fcitx5 (README, "Test"):
#   1. the engine (cargo) and the harness (cmake), warnings as errors
#   2. ctest: the key-table cases, the transcripts, the core, and the addon in a headless Fcitx5
#   3. the core once more with ASan and UBSan
#   4. an install into a staging folder, and a fresh fcitx5 on it, driven over D-Bus
# Nothing of the user's is touched: each step has its own session bus, home and runtime folder.
#
#   tools/test.sh             everything
#   BUILD=<dir> tools/test.sh  build somewhere else (default: build/ here)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD:-$here/build}
jobs=$(nproc)

ibus=ON
pkg-config --exists ibus-1.0 || { ibus=OFF; echo "(no ibus-1.0 headers: the IBus frontend isn't built)"; }

echo "== build"
cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
  -DBADA_WERROR=ON -DBADA_IBUS=$ibus > /dev/null
cmake --build "$build" -j"$jobs"

echo "== tests"
# Its own session bus: the state's D-Bus signal is checked, and the user's bus hears nothing.
dbus-run-session -- ctest --test-dir "$build" --output-on-failure

echo "== core tests with ASan and UBSan"
cmake -S "$here" -B "$build/asan" -DCMAKE_BUILD_TYPE=Debug -DBADA_WERROR=ON -DBADA_FCITX5=OFF -DBADA_IBUS=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" > /dev/null
cmake --build "$build/asan" -j"$jobs" > /dev/null
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 dbus-run-session -- ctest --test-dir "$build/asan" --output-on-failure

echo "== install into a staging folder"
stage=$build/stage
rm -rf "$stage"
DESTDIR=$stage cmake --install "$build" > /dev/null
(cd "$stage" && find . -type f | sort)
test -f "$stage/usr/share/bada/data/kanji.bin" || { echo "the engine's data isn't installed"; exit 1; }

echo "== a fresh fcitx5 on the staged install, over D-Bus"
sh "$here/tools/e2e-dbus.sh" "$stage/usr"

echo "== all passed"
