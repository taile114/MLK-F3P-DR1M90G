#!/bin/sh
set -u
ulimit -c 0 2>/dev/null || true
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backend=${1:-CpuRef}
case "$backend" in CpuRef|CpuAcc|CpuAcc,CpuRef) ;; *) echo 'Usage: sh run_native_layers.sh [CpuRef|CpuAcc|CpuAcc,CpuRef]' >&2; exit 2;; esac
runtime=${F3P_PROBE_ROOT:-/root/f3p-rul-probe}
if [ ! -x "$root/bin/native_layers" ]; then echo "Missing executable: $root/bin/native_layers" >&2; exit 2; fi
if [ ! -e "$runtime/lib/libarmnn.so.32" ]; then echo "Missing vendor runtime: $runtime/lib/libarmnn.so.32" >&2; exit 2; fi
export LD_LIBRARY_PATH="$runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
results="$root/results_${backend}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "$results" || exit 2
if command -v ldd >/dev/null 2>&1; then ldd "$root/bin/native_layers" > "$results/ldd.txt" 2>&1 || true; fi
echo "results=$results"
for name in relu less greater_equal broadcast_less logical_and comparison_and cast_bool_float float_less_via_ceil float_greater_equal_via_ceil float_less_boundaries spline_gate_float batch_matmul_2d batch_matmul_3d einsum_via_fc reduce_l2_decomposed; do
    status=0
    F3P_LAYER_GRAPH="$results/$name.dot" "$root/bin/native_layers" "$name" "$backend" > "$results/$name.log" 2>&1 || status=$?
    echo "--- $name exit_code=$status ---"
    if [ "$status" -eq 0 ]; then tail -n 1 "$results/$name.log"; else tail -n 8 "$results/$name.log"; fi
    printf '%s %s\n' "$name" "$status" >> "$results/summary.txt"
done
echo '--- summary ---'
cat "$results/summary.txt"
echo "results=$results"
