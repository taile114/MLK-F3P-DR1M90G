#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${CXX:?Set paired vendor compiler}"
: "${ARMNN_ROOT:?Set paired ArmNN include/lib root}"
mkdir -p "$root/bin"
for target in fused reference_bench; do
    "$CXX" -std=c++17 -O3 -ffp-contract=off -Wall -Wextra -Wpedantic \
        -I"$ARMNN_ROOT/include" -I"$root/src" "$root/src/native_rul_$target.cpp" \
        -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" -larmnn -pthread -ldl \
        -o "$root/bin/rul_$target"
done
