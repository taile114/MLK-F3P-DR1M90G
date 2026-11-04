#!/bin/sh
set -eu
ulimit -c 0 2>/dev/null || true
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
group=${1:-control}
case "$group" in control|smoke|matrix) ;; *) echo 'Usage: sh run.sh [control|smoke|matrix]' >&2; exit 2;; esac
runtime=${F3P_ARMNN_LIB:-/opt/armnn_lib/lib}
test -e "$runtime/libarmnn.so.32" || { echo "Missing runtime: $runtime" >&2; exit 2; }
test -x "$root/bin/npu_linear" || { echo 'Missing npu_linear executable' >&2; exit 2; }
command -v timeout >/dev/null 2>&1 || { echo 'A timeout command is required for hardware controls' >&2; exit 2; }
test -c /dev/hard_npu && test -c /dev/soft_npu && test -c /dev/cma_mem || { echo 'NPU/CMA devices missing' >&2; exit 2; }
export LD_LIBRARY_PATH="$runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
results="$root/results_${group}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir "$results"
uname -a > "$results/kernel.txt"
cat /proc/cmdline > "$results/cmdline.txt"
sha256sum "$runtime/libarmnn.so.32.1" > "$results/runtime.sha256"
if command -v ldd >/dev/null 2>&1; then ldd "$root/bin/npu_linear" > "$results/ldd.txt" 2>&1 || true; fi
printf 'shape\tmode\tbackend\texit_code\n' > "$results/summary.tsv"
snapshot() {
    cat /proc/interrupts > "$1/interrupts_$2.txt"
    cat /proc/meminfo > "$1/meminfo_$2.txt"
    dmesg > "$1/dmesg_$2.txt"
}
snapshot "$results" before
echo "results=$results runtime=$runtime group=$group"
passed=0
unsupported=0
halt=0
while read -r name rows inputs outputs; do
    case "$name" in ''|'#'*) continue;; esac
    modes='fc conv conv_pad8'
    if [ "$group" = control ]; then modes='fc conv'; fi
    for mode in $modes; do
        pair="$results/${name}_${mode}"
        mkdir "$pair"
        snapshot "$pair" before
        for backend in CpuRef Alnpu; do
            folder="$pair/$backend"
            mkdir "$folder"
            status=0
            timeout -k 5 30 "$root/bin/npu_linear" "$mode" "$rows" "$inputs" "$outputs" "$backend" "$folder" \
                > "$folder/probe.log" 2>&1 || status=$?
            printf '%s\n' "$status" > "$folder/exit_code.txt"
            printf '%s\t%s\t%s\t%s\n' "$name" "$mode" "$backend" "$status" >> "$results/summary.tsv"
            echo "--- shape=$name B=$rows I=$inputs O=$outputs mode=$mode backend=$backend exit_code=$status ---"
            if [ "$status" -eq 0 ]; then
                passed=$((passed + 1))
                grep -E 'compute_layer_type=|^result=' "$folder/probe.log" || true
            elif [ "$backend" = Alnpu ] && [ "$status" -eq 40 ]; then
                unsupported=$((unsupported + 1))
                tail -n 12 "$folder/probe.log"
            else
                halt=$status
                cat "$folder/probe.log"
                break
            fi
        done
        snapshot "$pair" after
        if [ "$halt" -ne 0 ]; then break; fi
    done
    if [ "$halt" -ne 0 ]; then break; fi
done < "$root/${group}_shapes.tsv"
snapshot "$results" after
echo '--- interrupt snapshots ---'
grep -i npu "$results/interrupts_before.txt" || true
grep -i npu "$results/interrupts_after.txt" || true
if [ "$halt" -ne 0 ]; then
    echo "result=stopped exit_code=$halt results=$results"
    exit "$halt"
fi
echo "result=capability_survey_completed passed_backend_runs=$passed optimize_rejections=$unsupported full_rul_npu_verified=false results=$results"
