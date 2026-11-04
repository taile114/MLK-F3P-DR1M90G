#!/usr/bin/env bash
# Rebuild the Linux kernel image with the vendor NPU drivers built in.
#
# The CPU/NPU hybrid backend needs /dev/hard_npu, /dev/soft_npu and
# /dev/cma_mem. Hard NPU executes the FullyConnected branches, soft NPU does
# the data transforms the vendor backend issues, and both exchange buffers
# through cma_mem. The vendor
# kernel tree ships the three drivers under drivers/npu/, but the board
# .config has
# CONFIG_NPU_MODULE unset, so no node is created. This script enables the menu
# plus the hard NPU, soft NPU and CMA drivers (built-in, so no module install is
# needed)
# and republishes uImage.lz4 to both output/ (the staging copy that
# build_boot_bin.sh re-copies) and images/boot/ (what burn_sd.sh packs).
set -euo pipefail
FLASH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR="$FLASH_DIR/vendor"
KDIR="$VENDOR/sources/linux"
CROSS="$VENDOR/tools/aarch64-linux/bin/aarch64-linux-gnu-"
MKIMAGE="$VENDOR/sources/u-boot/tools/mkimage"
OUT_DIR="$VENDOR/boards/DR1M/buildroot/output"
BOOT_DIR="$VENDOR/boards/DR1M/buildroot/images/boot"

[[ -f "$KDIR/.config" ]] || { echo "ERROR: kernel .config missing: $KDIR/.config" >&2; exit 1; }
[[ -x "$CROSS"gcc ]] || { echo "ERROR: cross toolchain missing: ${CROSS}gcc" >&2; exit 1; }
[[ -x "$MKIMAGE" ]] || { echo "ERROR: mkimage missing: $MKIMAGE" >&2; exit 1; }
command -v lz4 >/dev/null 2>&1 || {
  echo "ERROR: lz4 is not on PATH (try: nix shell nixpkgs#lz4 -c bash ./build_kernel_npu.sh)" >&2
  exit 1
}

echo "== 1) enable NPU drivers in $KDIR/.config =="
"$KDIR/scripts/config" --file "$KDIR/.config" \
  --enable NPU_MODULE \
  --enable HARD_NPU_DRIVER \
  --enable SOFT_NPU_DRIVER \
  --enable CMA_MEM_DRIVER \
  --enable WIRELESS --enable WLAN --enable CFG80211 --enable WEXT_CORE \
  --enable CFG80211_WEXT --enable RFKILL \
  --enable USB_SERIAL --enable USB_SERIAL_WWAN --enable USB_SERIAL_OPTION \
  --enable USB_NET_DRIVERS --enable USB_USBNET --enable USB_WDM \
  --enable USB_NET_QMI_WWAN --enable USB_NET_CDCETHER --enable USB_NET_CDC_NCM \
  --enable USB_NET_CDC_MBIM --enable USB_NET_RNDIS_HOST \
  --enable PPP --enable PPP_ASYNC --enable PPP_DEFLATE --enable PPP_BSDCOMP --enable PPPOE
make -s -C "$KDIR" ARCH=arm64 CROSS_COMPILE="$CROSS" olddefconfig

for symbol in CONFIG_NPU_MODULE CONFIG_HARD_NPU_DRIVER CONFIG_SOFT_NPU_DRIVER \
        CONFIG_CMA_MEM_DRIVER CONFIG_CFG80211 CONFIG_USB_SERIAL_OPTION CONFIG_USB_NET_QMI_WWAN \
        CONFIG_PPP CONFIG_PPPOE CONFIG_PPP_ASYNC; do
  grep -q "^$symbol=y" "$KDIR/.config" || {
    echo "ERROR: $symbol is not =y after olddefconfig" >&2
    exit 1
  }
done

echo "== 2) build Image =="
make -C "$KDIR" ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$(nproc)" Image

echo "== 3) wrap Image as uImage.lz4 =="
lz4 -f -9 "$KDIR/arch/arm64/boot/Image" "$KDIR/arch/arm64/boot/Image.lz4"
"$MKIMAGE" -A arm64 -O linux -T kernel -C lz4 -a 0x00400000 -e 0x00400000 -n Linux \
  -d "$KDIR/arch/arm64/boot/Image.lz4" "$KDIR/arch/arm64/boot/uImage.lz4"

cp -a "$KDIR/arch/arm64/boot/uImage.lz4" "$OUT_DIR/uImage.lz4"
cp -a "$OUT_DIR/uImage.lz4" "$BOOT_DIR/uImage.lz4"
echo "kernel -> $OUT_DIR/uImage.lz4"
echo "kernel -> $BOOT_DIR/uImage.lz4"
echo "NPU drivers: hard_npu (/dev/hard_npu), soft_npu (/dev/soft_npu), cma_mem (/dev/cma_mem) built in"
