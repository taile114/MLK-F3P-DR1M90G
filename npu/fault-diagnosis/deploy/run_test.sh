#!/bin/sh
# BusyBox-compatible offline test. NPU failures are retained and never retried.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-hybrid_split}
count=${2:-360}
case "$mode" in cpu|quantized|armnn_ref|hybrid|hybrid_split) :;; *) echo 'mode must be cpu, quantized, armnn_ref, hybrid or hybrid_split'; exit 2;; esac
case "$count" in ''|*[!0-9]*) exit 2;; esac
[ "$count" -ge 1 ] && [ "$count" -le 360 ] || exit 2
export LD_LIBRARY_PATH="${ARMNN_LIB:-/opt/armnn_lib/lib}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export OMP_NUM_THREADS=1
cd "$root"
sha256sum -c checksums.sha256 > /dev/null
if [ "$mode" = hybrid ] || [ "$mode" = hybrid_split ]; then
    for node in /dev/hard_npu /dev/soft_npu /dev/cma_mem; do [ -e "$node" ] || { echo "MISSING $node"; exit 3; }; done
fi
result="$root/results_${mode}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir "$result"
uname -a > "$result/uname.txt"
cat /proc/cmdline > "$result/cmdline.txt"
cat /proc/meminfo > "$result/meminfo_before.txt"
cat /proc/interrupts > "$result/interrupts_before.txt"
sha256sum "${ARMNN_LIB:-/opt/armnn_lib/lib}/libarmnn.so.32.1" > "$result/runtime.sha256"
sha256sum "$root/bin/fault_test" > "$result/program.sha256"
cp checksums.sha256 "$result/assets.sha256"
dmesg > "$result/dmesg_before.txt"
status=0
"$root/bin/timeout" -k 5 900 "$root/bin/fault_test" "$root" "$mode" "$count" "$result" > "$result/probe.log" 2>&1 || status=$?
printf '%s\n' "$status" > "$result/exit_code.txt"
cat /proc/meminfo > "$result/meminfo_after.txt"
cat /proc/interrupts > "$result/interrupts_after.txt"
dmesg > "$result/dmesg_after.txt"
tail -n 8 "$result/probe.log"
echo "exit_code=$status results=$result"
sync
exit "$status"
