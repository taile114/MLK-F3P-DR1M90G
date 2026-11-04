#!/usr/bin/env bash
# Build and stage real-data userspace and its matching kernel module.
set -euo pipefail
flash="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$flash/../.." && pwd)"
export CROSS_COMPILE="${CROSS_COMPILE:-$flash/vendor/tools/aarch64-linux/bin/aarch64-linux-gnu-}"
export ARCH=arm64
kdir="${KDIR:-$flash/vendor/sources/linux}"
"$repo/host/ps-local/build_arm.sh"
make -C "$repo/host/ps-local/driver" KDIR="$kdir" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE"
release="$(make -s -C "$kdir" kernelrelease ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE")"
bundle="$flash/rootfs-overlay/opt/qt_play"
mkdir -p "$bundle" "$flash/rootfs-overlay/lib/modules/$release/extra" "$flash/rootfs-overlay/usr/bin"
install -m755 "$repo/host/ps-local/build-arm/mlk_ps_local" "$bundle/mlk_ps_local"
install -m755 "$repo/host/ps-local/build-arm/mlk-fault" "$flash/rootfs-overlay/usr/bin/mlk-fault"
install -m755 "$repo/host/ps-local/build-arm/plctl" "$flash/rootfs-overlay/usr/bin/plctl"
install -m755 "$repo/host/ps-local/build-arm/capture-tool/mlk-capture" "$bundle/mlk-capture"
install -m644 "$repo/host/ps-local/driver/pl_ctrl.ko" "$flash/rootfs-overlay/lib/modules/$release/extra/pl_ctrl.ko"
install -m755 "$repo/host/ps-local/run_arm_app.sh" "$bundle/run_arm_app.sh"
# Ship the bitstreams so a board can load them with the FPGA manager without re-burning.
# The names must match the current TD top, not an earlier one: this list is what
# the rootfs carries. Override PL_BITS when the merged top writes other names
# (the unified RS485 top, for example, lands in fpga/uisrc/out/td/fpga_top.bit).
fw="$flash/rootfs-overlay/lib/firmware"
mkdir -p "$fw"
for bit in ${PL_BITS:-soc_hdmi board_test}; do
    src="${PL_BIT_DIR:-$repo/fpga/uisrc/out}/$bit/$bit.bit"
    [[ -f "$src" ]] || { echo "Bitstream missing: $src (run the TD build first, or set PL_BITS/PL_BIT_DIR)" >&2; exit 1; }
    if [[ "$bit" == soc_hdmi ]]; then
        python3 "$repo/fpga/soc/verify_npu_bit.py" "$(dirname "$src")"
        install -m644 "${src%.bit}.npu.sha256" "$fw/soc_hdmi.npu.sha256"
    fi
    install -m644 "$src" "$fw/$bit.bit"
done
# Existing Qt runtime and CJK fonts are required.
[[ -f "$bundle/qt_lib/lib/libQt5Core.so.5" ]] || {
    echo "Qt runtime missing: copy host/vendor/qt/qt_play/qt_lib into $bundle" >&2; exit 1;
}
echo "Staged ps-local and pl_ctrl for kernel $release"
if [[ -n "${RUL_BUNDLE:-}" ]]; then
    "$flash/stage_rul.sh"
fi
if [[ -n "${MLK_FAULT_ROOT:-}" ]]; then
    bash "$flash/stage_fault.sh"
fi
