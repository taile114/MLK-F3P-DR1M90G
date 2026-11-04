#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${CXX:?Set the paired F3P AArch64 compiler}"
: "${ARMNN_ROOT:?Set the paired vendor Arm NN include/lib directory}"
mkdir -p "$root/bin"
"$CXX" -std=c++17 -O3 -ffp-contract=off -Wall -Wextra -Wpedantic \
    -I"$ARMNN_ROOT/include" "$root/src/fault_test.cpp" \
    -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" -larmnn -pthread -ldl \
    -o "$root/bin/fault_test"
"$CXX" --version > "$root/compiler.txt"
