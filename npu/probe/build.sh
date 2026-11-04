#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
armnn="$root/vendor/armnn"
if [ ! -f "$armnn/include/armnn/ArmNN.hpp" ] || [ ! -f "$armnn/lib/libarmnn.so" ]; then
    mkdir -p "$armnn"
    tar xzf "$root/vendor/armnn.tar.gz" -C "$armnn"
fi
if [ -n "${CXX:-}" ]; then
    compiler=$CXX
elif [ "$(uname -m)" = "aarch64" ] && command -v g++ >/dev/null 2>&1; then
    compiler=g++
elif command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
    compiler=aarch64-linux-gnu-g++
else
    echo 'Set CXX to the vendor tools/aarch64-linux/bin/aarch64-linux-gnu-g++ executable.' >&2
    exit 2
fi
machine=$("$compiler" -dumpmachine)
case "$machine" in aarch64*linux*) ;; *) echo "Unsupported compiler target: $machine" >&2; exit 2;; esac
mkdir -p "$root/bin" "$root/lib"
cp -P "$armnn"/lib/*.so* "$root/lib/"
"$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -I"$armnn/include" "$root/src/rul_probe.cpp" \
    -L"$armnn/lib" -Wl,-rpath-link,"$armnn/lib" \
    '-Wl,-rpath,$ORIGIN/../lib' \
    -larmnnOnnxParser -larmnn -lprotobuf -pthread -ldl \
    -o "$root/bin/rul_probe"
echo "Built $root/bin/rul_probe with $compiler ($machine)"
if command -v file >/dev/null 2>&1; then file "$root/bin/rul_probe"; fi
