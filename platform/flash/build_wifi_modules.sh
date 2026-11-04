#!/usr/bin/env bash
# Build the vendor USB WiFi driver without its fixed 1000-buffer RX pool.
set -euo pipefail
flash="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$flash/../.." && pwd)"
source_dir="${AIC_SOURCE:-$repo/docs/MLK-F3P-DR1M90G-Resource/01_start/02_start_Linux/05_application_demo/05-4_无线通信方案/WIFI/aic8800}"
build="$flash/vendor/network/aic8800"
kernel="$flash/vendor/sources/linux"
cross="$flash/vendor/tools/aarch64-linux/bin/aarch64-linux-gnu-"
[[ -f "$source_dir/aic8800_fdrv/aicwf_usb.c" ]] || { echo "Missing AIC8800 source: $source_dir" >&2; exit 1; }
mkdir -p "$build"
# Keep generated objects out of the supplied resource tree.
tar -C "$source_dir" --exclude='*.o' --exclude='*.ko' --exclude='*.a' --exclude='.*.cmd' --exclude='*.mod*' --exclude=Module.symvers --exclude=modules.order -cf - . | tar -C "$build" -xf -
# Building Image alone leaves the SDK's old Module.symvers in place.
if [[ "$kernel/vmlinux.symvers" -nt "$kernel/Module.symvers" ]]; then
    make -C "$kernel" ARCH=arm64 CROSS_COMPILE="$cross" modules -j"${JOBS:-4}"
fi
make -C "$kernel" M="$build" ARCH=arm64 CROSS_COMPILE="$cross" \
    CONFIG_PLATFORM_ROCKCHIP=n CONFIG_PREALLOC_RX_SKB=n CONFIG_PREALLOC_TXQ=n modules -j"${JOBS:-4}"
out="${NETWORK_ASSETS:-$flash/vendor/boards/DR1M/buildroot/images/boot}"
install -m644 "$build/aic_load_fw/aic_load_fw.ko" "$out/aic_load_fw.ko"
install -m644 "$build/aic8800_fdrv/aic8800_fdrv.ko" "$out/aic8800_fdrv.ko"
echo "Built AIC8800 modules with dynamic RX buffers."
