#!/bin/sh
set -u
ulimit -c 0 2>/dev/null || true
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
name=${1:-u8}
case "$name" in inspect|u8|u8_s8|s8) ;; *) echo 'Usage: sh run.sh [inspect|u8|u8_s8|s8]' >&2; exit 2;; esac
runtime=${F3P_ARMNN_LIB:-/opt/armnn_lib/lib}
test -e "$runtime/libarmnn.so.32" || { echo "Missing runtime: $runtime" >&2; exit 2; }
test -x "$root/bin/npu_smoke" || { echo 'Missing npu_smoke executable' >&2; exit 2; }
export LD_LIBRARY_PATH="$runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
results="$root/results_${name}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "$results" || exit 2
cat /proc/interrupts > "$results/interrupts_before.txt"
dmesg > "$results/dmesg_before.txt"
if command -v ldd >/dev/null 2>&1; then ldd "$root/bin/npu_smoke" > "$results/ldd.txt" 2>&1 || true; fi
sha256sum "$runtime/libarmnn.so.32.1" > "$results/runtime.sha256" 2>&1 || true
uname -a > "$results/kernel.txt"
cat /proc/cmdline > "$results/cmdline.txt"
echo "results=$results runtime=$runtime"
if [ "$name" = inspect ]; then backends=CpuRef; else backends='CpuRef Alnpu'; fi
final=0
for backend in $backends; do
    mkdir "$results/$backend" || exit 2
    status=0
    if command -v timeout >/dev/null 2>&1; then
        timeout -k 5 30 "$root/bin/npu_smoke" "$name" "$backend" "$results/$backend" > "$results/$backend/probe.log" 2>&1 || status=$?
    else
        "$root/bin/npu_smoke" "$name" "$backend" "$results/$backend" > "$results/$backend/probe.log" 2>&1 || status=$?
    fi
    printf '%s\n' "$status" > "$results/$backend/exit_code.txt"
    echo "--- backend=$backend case=$name exit_code=$status ---"
    cat "$results/$backend/probe.log"
    printf '%s %s %s\n' "$name" "$backend" "$status" >> "$results/summary.txt"
    if [ "$status" -ne 0 ]; then final=$status; fi
    if [ "$backend" = CpuRef ] && [ "$status" -ne 0 ]; then echo 'CPU control failed; NPU run skipped'; break; fi
done
cat /proc/interrupts > "$results/interrupts_after.txt"
dmesg > "$results/dmesg_after.txt"
echo '--- NPU interrupt snapshots ---'
grep -i npu "$results/interrupts_before.txt" || true
grep -i npu "$results/interrupts_after.txt" || true
echo "exit_code=$final results=$results"
exit "$final"
