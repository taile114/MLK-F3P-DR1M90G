#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${CXX:?Set vendor compiler}"
: "${ARMNN_ROOT:?Set matching ArmNN include/lib root}"
mkdir -p "$root/bin"
"$CXX" -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Wpedantic \
    -I"$ARMNN_ROOT/include" -I"$root/src" "$root/src/native_rul_cma.cpp" \
    -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" -larmnn -pthread -ldl -o "$root/bin/rul_cma"
