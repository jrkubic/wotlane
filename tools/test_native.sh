#!/bin/sh
# ABOUTME: macOS/Linux counterpart of tools\test_native.bat: configures native/tests in native/tests/build, builds and runs the native
# ABOUTME: layer's unit tests (frame interpolation, settings, mods, deterministic math). Exit status 0 when every test passes.
set -eu
. "$(dirname "$0")/posix_env.sh"

case ${1-} in
    -h|--help) echo "usage: sh tools/test_native.sh   (full build log: native/tests/build/build.log)"; exit 0 ;;
esac

build=$REPO/native/tests/build
if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$REPO/native/tests" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || fail "configure failed"
fi
cmake --build "$build" >"$build/build.log" 2>&1 || {
    grep -E 'error' "$build/build.log" | head -10 >&2
    fail "build failed; full log: $build/build.log"
}
"$build/wotm_native_test"
