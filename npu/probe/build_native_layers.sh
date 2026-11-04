#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${ARMNN_ROOT:?Set ARMNN_ROOT to the matching vendor armnn include/lib directory}"
: "${CXX:?Set CXX to the vendor aarch64-linux-gnu-g++}"
machine=$("$CXX" -dumpmachine)
case "$machine" in aarch64*linux*) ;; *) echo "Unsupported compiler target: $machine" >&2; exit 2;; esac
test -f "$ARMNN_ROOT/include/armnn/ArmNN.hpp"
test -f "$ARMNN_ROOT/lib/libarmnn.so"
mkdir -p "$root/bin"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -I"$ARMNN_ROOT/include" "$root/src/native_layers.cpp" \
    -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" \
    -larmnn -pthread -ldl -o "$root/bin/native_layers"
echo "Built $root/bin/native_layers ($machine)"
