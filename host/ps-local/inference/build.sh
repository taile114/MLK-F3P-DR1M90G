#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${CXX:?Set paired AArch64 compiler}"
: "${ARMNN_ROOT:?Set paired ArmNN include/lib root}"
"$CXX" -std=c++17 -O3 -ffp-contract=off -fPIC -shared -Wall -Wextra \
    -I"$ARMNN_ROOT/include" -I"$root/src" "$root/src/engine.cpp" \
    -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" -Wl,-z,defs \
    -larmnn -pthread -ldl -o "$root/libmlk_rul.so"
"$CXX" -std=c++17 -O2 -I"$root/src" "$root/src/check_engine.cpp" -ldl -o "$root/check_engine"
