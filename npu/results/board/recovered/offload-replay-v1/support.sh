#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH=/opt/armnn_lib/lib
results="$root/results_support_$(date +%Y%m%d_%H%M%S)_$$"
mkdir "$results"
cat /proc/interrupts > "$results/interrupts_before.txt"
sha256sum "$root/bin/npu_op_support" /opt/armnn_lib/lib/libarmnn.so.32.1 > "$results/program_runtime.sha256"
for name in relu sigmoid tanh softmax ceil add sub mul div batch_matmul; do
 mkdir "$results/$name"
 status=0
 "$root/bin/timeout" -k 5 20 "$root/bin/npu_op_support" "$name" "$results/$name" > "$results/$name/probe.log" 2>&1 || status=$?
 printf '%s %s\n' "$name" "$status" >> "$results/summary.txt"
 if [ "$status" -ne 0 ] && [ "$status" -ne 40 ]; then cat "$results/$name/probe.log"; exit "$status"; fi
done
cat /proc/interrupts > "$results/interrupts_after.txt"
cat "$results/summary.txt"
sync
echo "results=$results"
