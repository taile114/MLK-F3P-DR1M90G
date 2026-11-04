#!/bin/sh
# Read-only check. Run on the target image before deployment.
set -u
mode=${1:-cpu}
case "$mode" in cpu|hybrid) :;; *) echo 'usage: inspect_environment.sh cpu|hybrid' >&2; exit 2;; esac
missing=0
uname -a
for file in /dev/fb0 /opt/armnn_lib/lib/libarmnn.so.32.1; do
    if [ -e "$file" ]; then ls -l "$file"; else echo "missing: $file"; missing=1; fi
done
if [ "$mode" = hybrid ]; then
    for file in /dev/hard_npu /dev/soft_npu /dev/cma_mem; do
        if [ -e "$file" ]; then ls -l "$file"; else echo "missing: $file"; missing=1; fi
    done
fi
if [ -e /dev/pl_ctrl ]; then
    plctl probe || missing=1
else
    echo 'PL acquisition unavailable; dataset replay remains independent'
fi
grep -E 'MemTotal:|MemAvailable:|CmaTotal:|CmaFree:' /proc/meminfo
if [ -r /proc/device-tree/reserved-memory/sensor-dma-buffer@3ef00000/reg ]; then
    echo 'sensor DMA reserved-memory node present; verify reg and CMA ranges in the DTB build'
fi
ls -l /opt/qt_play/mlk_ps_local /opt/qt_play/qt_lib/lib/libQt5Core.so.5 2>/dev/null || true
exit "$missing"
