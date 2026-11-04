#!/bin/sh
# Read-only board inventory. This script does not load drivers or change FPGA configuration.
set -u
echo '=== system ==='
uname -a
getconf LONG_BIT 2>/dev/null || true
cat /etc/os-release 2>/dev/null || true
echo '=== memory ==='
grep -E 'MemTotal|MemAvailable|CmaTotal|CmaFree' /proc/meminfo 2>/dev/null || true
echo '=== devices used by the vendor library ==='
for node in /dev/hard_npu /dev/soft_npu /dev/cma_mem; do
    if [ -e "$node" ]; then ls -l "$node"; else echo "MISSING $node"; fi
done
echo '=== vendor libraries ==='
for base in /opt /usr/lib /usr/local/lib /root; do
    if [ -d "$base" ]; then
        find "$base" -maxdepth 4 \( -name 'libarmnn.so*' -o -name 'libarmnnOnnxParser.so*' -o -name 'libprotobuf.so*' \) 2>/dev/null || true
    fi
done
echo '=== network ==='
if command -v ip >/dev/null 2>&1; then ip -4 addr show; else ifconfig 2>/dev/null || true; fi
echo '=== available native compiler ==='
command -v g++ 2>/dev/null || true
echo '=== kernel NPU/CMA messages ==='
dmesg 2>/dev/null | grep -Ei 'npu|cma' | tail -n 60 || true
