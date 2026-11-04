#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${CXX:?Set CXX to the vendor aarch64-linux-gnu-g++}"
: "${ARMNN_ROOT:?Set ARMNN_ROOT to the matching include/lib directory}"
case "$("$CXX" -dumpmachine)" in aarch64*linux*) ;; *) echo 'AArch64 Linux compiler required' >&2; exit 2;; esac
mkdir -p "$root/bin"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic -ffp-contract=off \
    -I"$ARMNN_ROOT/include" "$root/src/npu_linear_padded.cpp" \
    -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" \
    -larmnn -pthread -ldl -o "$root/bin/npu_linear"
echo "Built $root/bin/npu_linear"
