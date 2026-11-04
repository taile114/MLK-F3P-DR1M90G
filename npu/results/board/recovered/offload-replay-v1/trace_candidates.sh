#!/bin/sh
set -eu
root=/mnt/mmcblk0p2/npu-tests/offload-v2/f3p-offload-v2
cd "$root"
export LD_LIBRARY_PATH=/opt/armnn_lib/lib F3P_TRACE_OPS=1 F3P_VERIFY_INT=0
for name in ff4 input_ff5; do
 out="$root/results_trace_${name}_$(date +%Y%m%d_%H%M%S)_$$"
 mkdir "$out"
 export F3P_QUANT_PLAN="qparams_${name}.tsv"
 unset F3P_CONV_PLAN
 if [ "$name" = input_ff5 ]; then export F3P_CONV_PLAN=conv_input.txt; fi
 cat /proc/interrupts > "$out/interrupts_before.txt"
 status=0
 "$root/bin/timeout" -k 5 90 "$root/bin/rul_offload" "$root" 0 "$out" hybrid > "$out/probe.log" 2>&1 || status=$?
 printf '%s\n' "$status" > "$out/exit_code.txt"
 cat /proc/interrupts > "$out/interrupts_after.txt"
 cat "$out/probe.log"
 echo "trace_directory=$out"
 if [ "$status" -ne 0 ]; then exit "$status"; fi
done
sync
