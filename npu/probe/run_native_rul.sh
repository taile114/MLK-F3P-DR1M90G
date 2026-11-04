#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
count=${1:-1}
backend=${2:-CpuRef}
case "$count" in ''|*[!0-9]*) echo 'Usage: sh run_native_rul.sh [count] [CpuRef|CpuAcc|CpuAcc,CpuRef]; count=0 means all 165' >&2; exit 2;; esac
case "$backend" in CpuRef|CpuAcc|CpuAcc,CpuRef) ;; *) echo 'Invalid CPU backend preference' >&2; exit 2;; esac
export F3P_CPU_BACKENDS="$backend"
runtime=${F3P_PROBE_ROOT:-/root/f3p-rul-probe}
export LD_LIBRARY_PATH="$runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$root/bin/rul_native" ]; then echo 'Missing bin/rul_native' >&2; exit 2; fi
results="$root/results_${backend}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "$results"
if command -v ldd >/dev/null 2>&1; then ldd "$root/bin/rul_native" > "$results/ldd.txt" 2>&1 || true; fi
status=0
"$root/bin/rul_native" "$root" "$count" "$results" > "$results/probe.log" 2>&1 || status=$?
cat "$results/probe.log"
echo "exit_code=$status results=$results"
exit "$status"
