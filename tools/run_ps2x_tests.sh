#!/bin/sh
# Builds and runs PS2Recomp's own test suite (ps2xTest, target ps2x_tests) against the patched clone (macOS/Linux).
# Use it after changing the runtime patch, e.g. ps2_runtime_macros.h or a ps2xTest case the patch adds. The game build
# (build_dev) turns the suite off, so it gets its own folder, <runtime>/build_tests. Prints the totals and the names of the
# failing tests; the full output is in <runtime>/logs/ps2x_tests.log.
set -eu
. "$(dirname "$0")/posix_env.sh"

usage() {
    cat <<EOF
usage: sh tools/run_ps2x_tests.sh [--runtime <dir>]
  --runtime  runtime folder (default: WOTM_RECOMP_DIR, .wotm_recomp_dir, or <repo>/runtime)
Exit status: 0 when every test passes, 1 otherwise.
EOF
}

rt=$(runtime_dir)
while [ $# -gt 0 ]; do
    case $1 in
        -h|--help) usage; exit 0 ;;
        --runtime) rt=$2; shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done

pr=$rt/PS2Recomp
bdir=$rt/build_tests
logs=$rt/logs
[ -d "$pr/.git" ] || fail "no PS2Recomp clone at $pr (run sh tools/setup_runtime.sh)"
mkdir -p "$logs"

if [ ! -f "$bdir/build.ninja" ]; then
    # the suite also tests the recompiler and the analyzer, so those are built here (they are off in build_dev)
    cmake -S "$pr" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWOTM_NATIVE_DIR="$REPO/native" -DPS2X_FAST_LINK=ON \
        -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_ANALYZER=ON -DPS2X_BUILD_TEST=ON -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_DEBUG_UI=OFF \
        -DPS2X_RUNNER_DIR="$REPO/recomp/retail" -B "$bdir" >"$logs/ps2x_tests_configure.log" 2>&1 ||
        fail "configure failed; see $logs/ps2x_tests_configure.log"
fi
cmake --build "$bdir" --target ps2x_tests >"$logs/ps2x_tests_build.log" 2>&1 || {
    grep -E 'error' "$logs/ps2x_tests_build.log" | head -10 >&2
    fail "build failed; full log: $logs/ps2x_tests_build.log"
}

status=0
(cd "$bdir" && ./ps2xTest/ps2x_tests) >"$logs/ps2x_tests.log" 2>&1 || status=$?
plain=$(sed 's/\x1b\[[0-9;]*m//g' "$logs/ps2x_tests.log")
printf '%s\n' "$plain" | grep -E '^(Total Tests|Passed|Failed):'
# a test's own log lines can come between its "[Run]: name" and its "[Failed]": name the last test started
printf '%s\n' "$plain" | awk '/\[Run\]: / { name = $0; sub(/.*\[Run\]: /, "", name); sub(/ *(\[(Passed|Failed)\]|\[[a-z:-]+\]).*$/, "", name) }
    /\[Failed\]/ && name != "" { print "  failed: " name; name = "" }'
echo "full output: $logs/ps2x_tests.log"
[ "$status" -eq 0 ] || exit 1
