#!/bin/sh
# ABOUTME: macOS/Linux counterpart of tools\build_net.bat: builds the netcode without the game (native/net: wotm_server and
# ABOUTME: wotm_net_test) in native/net/build and runs the tests. Exit status 0 when every test passes.
set -eu
. "$(dirname "$0")/posix_env.sh"

case ${1-} in
    -h|--help) echo "usage: sh tools/test_net.sh [notest]   (logs: native/net/build/build.log, test.log)"; exit 0 ;;
esac

build=$REPO/native/net/build
if [ ! -f "$build/build.ninja" ]; then
    cmake -S "$REPO/native/net" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || fail "configure failed"
fi
cmake --build "$build" >"$build/build.log" 2>&1 || {
    grep -E 'error|warning' "$build/build.log" | head -10 >&2
    fail "build failed; full log: $build/build.log"
}
grep -E 'warning' "$build/build.log" | head -5 >&2 || true
[ "${1-}" = notest ] && exit 0
status=0
"$build/wotm_net_test" >"$build/test.log" 2>&1 || status=$?
grep -v '^ok ' "$build/test.log" || true   # failures and the totals line; the full list is in test.log
exit "$status"
