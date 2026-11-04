#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-neon}
count=${2:-1}
case "$count" in ''|*[!0-9]*) exit 2;; esac
case "$mode" in reference|scalar|neon|cpu_quantized|hybrid|stem) :;; *) exit 2;; esac
export LD_LIBRARY_PATH="/opt/armnn_lib/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export F3P_VERIFY_INT=${F3P_VERIFY_INT:-1}
export F3P_PROFILE=0 F3P_CPU_BACKENDS=CpuAcc,CpuRef F3P_QUANT_PLAN=none
results="$root/results_${mode}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir "$results"
cat /proc/meminfo > "$results/meminfo_before.txt"
cat /proc/interrupts > "$results/interrupts_before.txt"
sha256sum /opt/armnn_lib/lib/libarmnn.so.32.1 > "$results/runtime.sha256"
sha256sum "$root/bin/rul_fused" "$root/bin/rul_reference_bench" > "$results/programs.sha256"
status=0
case "$mode" in
 reference) "$root/bin/timeout" -k 5 120 "$root/bin/rul_reference_bench" "$root" "$count" "$results" > "$results/probe.log" 2>&1 || status=$?;;
 stem) "$root/bin/timeout" -k 5 30 "$root/bin/rul_fused" --stem-check "$root/fixtures" "$results" > "$results/probe.log" 2>&1 || status=$?;;
 *) "$root/bin/timeout" -k 5 120 "$root/bin/rul_fused" "$root" "$count" "$results" "$mode" > "$results/probe.log" 2>&1 || status=$?;;
esac
printf '%s\n' "$status" > "$results/exit_code.txt"
cat /proc/meminfo > "$results/meminfo_after.txt"
cat /proc/interrupts > "$results/interrupts_after.txt"
dmesg > "$results/dmesg_after.txt"
tail -n 5 "$results/probe.log"
sync
echo "exit_code=$status results=$results"
exit "$status"
