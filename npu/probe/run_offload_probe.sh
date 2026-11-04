#!/bin/sh
# Independent experiment: frozen weights/scales, no silent CPU fallback.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-profile}
export LD_LIBRARY_PATH="/opt/armnn_lib/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export F3P_VERIFY_INT=${F3P_VERIFY_INT:-1}
unset F3P_TRACE_OPS F3P_CAPTURE_FC F3P_QUANT_PLAN F3P_CONV_PLAN
case "$mode" in
 profile|neon|cpu_quantized|hybrid)
    count=${2:-1}
    case "$count" in ''|*[!0-9]*) exit 2;; esac
    plan=${3:-qparams_head.tsv}
    case "$plan" in qparams_*.tsv) :;; *) exit 2;; esac
    case "$plan" in *[!A-Za-z0-9_.-]*) exit 2;; esac
    if [ "$mode" = profile ]; then
        export F3P_TRACE_OPS=1 F3P_CAPTURE_FC=1
        executable_mode=neon
    else executable_mode=$mode
    fi
    if [ "$mode" = hybrid ] || [ "$mode" = cpu_quantized ]; then
        test -f "$root/$plan"
        export F3P_QUANT_PLAN=$plan
        if [ -n "${4:-}" ]; then
            case "$4" in conv_*.txt) :;; *) exit 2;; esac
            case "$4" in *[!A-Za-z0-9_.-]*) exit 2;; esac
            test -f "$root/$4"
            export F3P_CONV_PLAN=$4
        fi
    fi
    ;;
 branch)
    label=${2:?Branch label required}
    layout=${3:-fc}
    capture=${4:?Captured FP32 result directory required}
    case "$label" in ''|*[!A-Za-z0-9_.]*) exit 2;; esac
    case "$layout" in fc|conv) :;; *) exit 2;; esac
    test -f "$capture/capture_${label}_input.bin"
    ;;
 *) exit 2;;
esac
results="$root/results_${mode}_$(date +%Y%m%d_%H%M%S)_$$"
mkdir "$results"
cat /proc/meminfo > "$results/meminfo_before.txt"
cat /proc/interrupts > "$results/interrupts_before.txt"
sha256sum /opt/armnn_lib/lib/libarmnn.so.32.1 > "$results/runtime.sha256"
sha256sum "$root/bin/rul_offload" > "$results/program.sha256"
if [ "$mode" = hybrid ] || [ "$mode" = cpu_quantized ]; then
    cp "$root/$plan" "$results/quantization_plan.tsv"
    if [ -n "${F3P_CONV_PLAN:-}" ]; then cp "$root/$F3P_CONV_PLAN" "$results/convolution_plan.txt"; fi
fi
status=0
if [ "$mode" = branch ]; then
    "$root/bin/timeout" -k 5 30 "$root/bin/rul_offload" --branch "$root" "$capture" "$label" "$results" "$layout" > "$results/probe.log" 2>&1 || status=$?
else
    "$root/bin/timeout" -k 5 180 "$root/bin/rul_offload" "$root" "$count" "$results" "$executable_mode" > "$results/probe.log" 2>&1 || status=$?
fi
printf '%s\n' "$status" > "$results/exit_code.txt"
cat /proc/meminfo > "$results/meminfo_after.txt"
cat /proc/interrupts > "$results/interrupts_after.txt"
dmesg > "$results/dmesg_after.txt"
tail -n 8 "$results/probe.log"
sync
echo "exit_code=$status results=$results"
exit "$status"
