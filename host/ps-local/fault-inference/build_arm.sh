#!/bin/sh
# 构建单故障模型共享库与检查程序（AArch64 Linux）。
#
#   ARMNN_ROOT=<armnn 目录> CXX=aarch64-linux-gnu-g++ sh build_arm.sh
#
# 设置 ARMNN_ROOT 时启用后端 2（CPU+CNN NPU）：四个 3x3 卷积成为仅允许 Alnpu
# 的独立图，层间 ReLU/MaxPool 在 CPU，与离线测试包的 hybrid_split 计划一致。
# 未设置时只编译后端 1（CPU），后端 2 在 create 阶段返回明确错误。
# 板端工具链、Arm NN 版本与运行库须与离线板测相同。
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-aarch64-linux-gnu-g++}
OUT=${OUT:-$HERE/build-aarch64}
ARMNN_ROOT=${ARMNN_ROOT:-}
case "$("$CXX" -dumpmachine)" in
    aarch64*linux*) ;;
    *) echo 'AArch64 Linux compiler required' >&2; exit 2 ;;
esac
mkdir -p "$OUT"

if [ -n "$ARMNN_ROOT" ]; then
    "$CXX" -O3 -std=c++17 -ffp-contract=off -fPIC -shared -pthread -Wl,-z,defs \
        -DMLK_FAULT_HAVE_ARMNN -I"$ARMNN_ROOT/include" \
        -o "$OUT/libmlk_fault.so" "$HERE/engine/fault_engine.cpp" \
        -L"$ARMNN_ROOT/lib" -Wl,-rpath-link,"$ARMNN_ROOT/lib" -larmnn -ldl
else
    "$CXX" -O3 -std=c++17 -ffp-contract=off -fPIC -shared -pthread -Wl,-z,defs \
        -o "$OUT/libmlk_fault.so" "$HERE/engine/fault_engine.cpp"
fi
"$CXX" -O2 -std=c++17 -o "$OUT/fault_check" "$HERE/check/fault_check.cpp" -ldl
if [ -n "$ARMNN_ROOT" ]; then
    "$CXX" -O2 -std=c++17 -pthread -DMLK_FAULT_HAVE_ARMNN -I"$ARMNN_ROOT/include" \
        "$HERE/check/test_tensor_binding.cpp" -L"$ARMNN_ROOT/lib" \
        -Wl,-rpath-link,"$ARMNN_ROOT/lib" -larmnn -ldl -o "$OUT/test_tensor_binding"
fi

echo "built: $OUT/libmlk_fault.so $OUT/fault_check"
