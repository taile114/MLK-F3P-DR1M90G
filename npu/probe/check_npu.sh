#!/bin/sh
# Read-only BusyBox-compatible NPU, device-tree and persistent-storage inventory.
set -u
echo '=== kernel and boot arguments ==='
uname -a
cat /proc/cmdline
echo '=== root filesystem and free space ==='
df -h
cat /proc/mounts
echo '=== NPU and CMA devices ==='
for node in /dev/hard_npu /dev/soft_npu /dev/cma_mem; do
    if [ -e "$node" ]; then ls -l "$node"; else echo "MISSING $node"; fi
done
echo '=== available kernel configuration ==='
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz | grep -Ei 'NPU|CMA|CONTIG|DMA_SHARED_BUFFER' || true
else
    echo 'UNAVAILABLE /proc/config.gz'
    for config in "/boot/config-$(uname -r)" "/lib/modules/$(uname -r)/build/.config"; do
        if [ -r "$config" ]; then
            echo "CONFIG_FILE $config"
            grep -Ei 'NPU|CMA|CONTIG|DMA_SHARED_BUFFER' "$config" || true
        fi
    done
fi
echo '=== module filenames containing npu or cma ==='
if [ -d /lib/modules ]; then
    find /lib/modules -type f | while IFS= read -r path; do
        name=${path##*/}
        case "$name" in *npu*|*NPU*|*cma*|*CMA*) echo "$path";; esac
    done
fi
echo '=== builtin NPU/CMA module entries ==='
if [ -r "/lib/modules/$(uname -r)/modules.builtin" ]; then
    grep -Ei '/[^/]*(npu|cma)[^/]*[.]ko' "/lib/modules/$(uname -r)/modules.builtin" || true
fi
echo '=== registered platform drivers ==='
for path in /sys/bus/platform/drivers/*; do
    name=${path##*/}
    case "$name" in *npu*|*NPU*|*cma*|*CMA*) echo "$path";; esac
done
echo '=== device-tree nodes containing npu or cma ==='
if [ -d /proc/device-tree ]; then
    find -L /proc/device-tree -type d | while IFS= read -r path; do
        name=${path##*/}
        case "$name" in
            *npu*|*NPU*|*cma*|*CMA*)
                echo "NODE $path"
                for field in compatible status; do
                    if [ -r "$path/$field" ]; then
                        printf '%s=' "$field"
                        tr '\000' ' ' < "$path/$field"
                        printf '\n'
                    fi
                done
                ;;
        esac
    done
fi
echo '=== loaded modules ==='
lsmod 2>/dev/null || cat /proc/modules
echo '=== NPU/CMA kernel messages ==='
dmesg 2>/dev/null | grep -Ei 'npu|cma' | tail -n 60 || true
echo '=== CPU capabilities ==='
grep -E 'processor|model name|Features|CPU implementer|CPU part' /proc/cpuinfo || true
