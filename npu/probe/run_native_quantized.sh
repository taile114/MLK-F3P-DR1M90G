#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-baseline}
count=${2:-1}
case "$count" in ''|*[!0-9]*) exit 2;; esac
case "$mode" in
    baseline) export F3P_CPU_BACKENDS=CpuAcc,CpuRef; export F3P_QUANT_PLAN=none;;
    cpu_quantized) export F3P_CPU_BACKENDS=CpuAcc,CpuRef; export F3P_QUANT_PLAN=qparams_head.tsv;;
    hybrid) export F3P_CPU_BACKENDS=Alnpu,CpuAcc,CpuRef; export F3P_QUANT_PLAN=qparams_head.tsv;;
    *) exit 2;;
esac
export F3P_PROFILE=1
export PATH="$root/bin:$PATH"
export LD_LIBRARY_PATH="/opt/armnn_lib/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
results="$root/results_${mode}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir "$results"
cat /proc/interrupts > "$results/interrupts_before.txt"
sha256sum /opt/armnn_lib/lib/libarmnn.so.32.1 > "$results/runtime.sha256"
status=0
timeout -k 5 90 "$root/bin/rul_quantized" "$root" "$count" "$results" > "$results/probe.log" 2>&1 || status=$?
printf '%s\n' "$status" > "$results/exit_code.txt"
cat /proc/interrupts > "$results/interrupts_after.txt"
dmesg > "$results/dmesg_after.txt"
cat "$results/probe.log"
sync
echo "exit_code=$status results=$results"
exit "$status"
