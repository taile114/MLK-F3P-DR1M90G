#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PATH="$root/bin:$PATH"
export LD_LIBRARY_PATH="/opt/armnn_lib/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
group=${1:-first}
case "$group" in first|all) ;; *) exit 2;; esac
test -c /dev/hard_npu
test -c /dev/soft_npu
test -c /dev/cma_mem
results="$root/results_${group}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir "$results"
uname -a > "$results/kernel.txt"
sha256sum /opt/armnn_lib/lib/libarmnn.so.32.1 > "$results/runtime.sha256"
cat /proc/interrupts > "$results/interrupts_before.txt"
printf 'case\tbackend\texit_code\n' > "$results/summary.tsv"
halt=0
while read -r name; do
    for backend in CpuRef Alnpu; do
        directory="$results/${name}_${backend}"
        mkdir "$directory"
        status=0
        timeout -k 5 30 "$root/bin/real_weight" "$root/cases/$name" "$backend" "$directory" \
            > "$directory/probe.log" 2>&1 || status=$?
        printf '%s\n' "$status" > "$directory/exit_code.txt"
        printf '%s\t%s\t%s\n' "$name" "$backend" "$status" >> "$results/summary.tsv"
        echo "case=$name backend=$backend exit_code=$status"
        if [ "$status" -eq 0 ]; then
            grep -E 'compute_layer_type=|^result=' "$directory/probe.log"
        else
            cat "$directory/probe.log"
            halt=$status
            break
        fi
    done
    if [ "$halt" -ne 0 ]; then break; fi
done < "$root/${group}_cases.txt"
cat /proc/interrupts > "$results/interrupts_after.txt"
dmesg > "$results/dmesg_after.txt"
sync
echo "result=group_finished exit_code=$halt results=$results"
exit "$halt"
