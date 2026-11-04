#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${CXX:?Set vendor AArch64 compiler}"
: "${ARMNN_ROOT:?Set matching ArmNN headers and libraries}"
mkdir -p "$root/bin"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic -ffp-contract=off \
    -I"$ARMNN_ROOT/include" "$root/src/npu_real_weight.cpp" \
    -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" \
    -larmnn -pthread -ldl -o "$root/bin/real_weight"
