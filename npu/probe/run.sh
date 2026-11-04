#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-parse}
count=${2:-1}
case "$mode" in parse|cpu|mixed) ;; *) echo 'Usage: sh run.sh parse | cpu [count] | mixed [count]' >&2; exit 2;; esac
if [ ! -x "$root/bin/rul_probe" ]; then echo 'Build bin/rul_probe in the ARM64 development environment first.' >&2; exit 2; fi
run_dir="$root/results/${mode}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "$run_dir"
export LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if command -v ldd >/dev/null 2>&1; then ldd "$root/bin/rul_probe" > "$run_dir/ldd.txt" 2>&1 || true; fi
set -- --model "$root/models/rul_fp32.onnx"
if [ "$mode" = parse ]; then
    set -- "$@" --parse-only
else
    backends=CpuRef
    if [ "$mode" = mixed ]; then backends=Alnpu,CpuAcc,CpuRef; fi
    set -- "$@" --input "$root/data/input_f32.bin" --reference "$root/data/reference_f32.bin" \
        --labels "$root/data/labels_f32.bin" --backends "$backends" --count "$count" \
        --csv "$run_dir/predictions.csv" --graph "$run_dir/optimized.dot" --profile "$run_dir/profile.json"
fi
status=0
"$root/bin/rul_probe" "$@" > "$run_dir/probe.log" 2>&1 || status=$?
cat "$run_dir/probe.log"
echo "exit_code=$status results=$run_dir"
exit "$status"
