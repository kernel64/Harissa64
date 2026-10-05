#!/bin/bash
# Builds and runs the unit tests on the WSL targets:
#   build-gcc    little-endian x86-64, gcc
#   build-ppc64  big-endian 64-bit PowerPC, cross gcc, run under qemu-ppc64
# Usage: tests/scripts/run_wsl.sh   (from the repository root, inside WSL)
set -u
cd "$(dirname "$0")/../.."
status=0

run_target() {
    local name="$1" dir="$2"; shift 2
    echo "== $name"
    if ! cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@" > "$dir.configure.log" 2>&1; then
        echo "$name: CONFIGURE FAILED (see $dir.configure.log)"; status=1; return
    fi
    if ! cmake --build "$dir" > "$dir.build.log" 2>&1; then
        echo "$name: BUILD FAILED"; tail -20 "$dir.build.log"; status=1; return
    fi
    grep -i "warning" "$dir.build.log" | head -20
    local runner=""
    case "$name" in *ppc64*) runner="qemu-ppc64" ;; esac
    $runner "./$dir/h64test" --version
    if ! $runner "./$dir/h64test" --unit | tail -1; then status=1; fi
    if ! $runner "./$dir/h64test" --unit > /dev/null; then echo "$name: UNIT TESTS FAILED"; status=1; fi
}

mkdir -p build-gcc build-ppc64
run_target "gcc (little-endian)" build-gcc
run_target "ppc64 big-endian (qemu)" build-ppc64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ppc64be.cmake
exit $status
