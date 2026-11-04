#!/usr/bin/env bash
# Stage wireless userspace, AIC8800 firmware and the vendor cellular bundle.
set -euo pipefail
flash="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
br="$flash/vendor/sources/buildroot"
kdir="$flash/vendor/sources/linux"
cross="$flash/vendor/tools/aarch64-linux/bin/aarch64-linux-gnu-"
assets="${NETWORK_ASSETS:-$flash/vendor/boards/DR1M/buildroot/images/boot}"
stage="${1:?usage: stage_network.sh ROOTFS_DIRECTORY}"
[[ -d "$stage" && -f "$br/.config" ]] || { echo 'rootfs staging directory or Buildroot config missing' >&2; exit 1; }
"$flash/apply_sdk_local.sh"
"$flash/build_wifi_modules.sh"
for symbol in CONFIG_CFG80211 CONFIG_USB_SERIAL_OPTION CONFIG_USB_NET_QMI_WWAN; do
    grep -q "^$symbol=y" "$kdir/.config" || { echo "Kernel lacks $symbol; run build_kernel_npu.sh first" >&2; exit 1; }
done
release="$(make -s -C "$kdir" ARCH=arm64 CROSS_COMPILE="$cross" kernelrelease)"
for name in aic_load_fw.ko aic8800_fdrv.ko GobiNet.ko; do
    [[ -f "$assets/$name" ]] || { echo "Missing network module: $assets/$name" >&2; exit 1; }
    strings "$assets/$name" | grep "^vermagic=$release " >/dev/null || { echo "Module kernel mismatch: $name" >&2; exit 1; }
done
python3 - "$br/.config" "$flash/network.config" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]); lines = p.read_text().splitlines()
for entry in Path(sys.argv[2]).read_text().splitlines():
    key = entry.split('=')[0]
    lines = [line for line in lines if not line.startswith(key+'=') and line != '# '+key+' is not set']
    lines.append(entry)
p.write_text('\n'.join(lines)+'\n')
PY
make -C "$br" olddefconfig
# Buildroot's cached wrapper embeds the external toolchain's absolute path.
if ! "$br/output/host/bin/aarch64-linux-gnu-gcc" --version >/dev/null 2>&1; then
    make -C "$br" HOST_CFLAGS="-O2 -std=gnu11 -I$br/output/host/include" toolchain-external-custom-reconfigure
fi
# Repair cached SDK host tools when the extracted SDK was moved.
if [[ -f "$br/output/host/bin/tic" ]] && ! readelf -d "$br/output/host/bin/tic" | grep -F "$br/output/host/lib" >/dev/null; then
    make -C "$br" HOST_CFLAGS="-O2 -std=gnu11 -I$br/output/host/include" host-ncurses-reconfigure
fi
for cached in "$br"/output/build/host-m4-*/Makefile; do
    [[ -f "$cached" ]] || continue
    if ! grep '^CFLAGS =.*-std=gnu11' "$cached" >/dev/null; then
        make -C "$br" host-m4-dirclean
    fi
done
# Buildroot 2022 host packages require pre-C23 C semantics with modern GCC.
make -C "$br" HOST_CFLAGS="-O2 -std=gnu11 -I$br/output/host/include" wpa_supplicant wireless-regdb chrony
mkdir -p "$stage/usr/sbin" "$stage/usr/lib" "$stage/lib/modules/$release/extra" "$stage/lib/firmware/aic8800D80"
for name in wpa_supplicant wpa_cli wpa_passphrase; do
    install -m755 "$br/output/target/usr/sbin/$name" "$stage/usr/sbin/$name"
done
for lib in "$br/output/target/usr/lib/"libnl*.so*; do cp -a "$lib" "$stage/usr/lib/"; done
for lib in "$br/output/target/usr/lib/"libssl.so* "$br/output/target/usr/lib/"libcrypto.so*; do cp -a "$lib" "$stage/usr/lib/"; done
cp -a "$br/output/target/lib/firmware/"regulatory.db* "$stage/lib/firmware/"
for name in aic_load_fw.ko aic8800_fdrv.ko GobiNet.ko; do
    install -m644 "$assets/$name" "$stage/lib/modules/$release/extra/$name"
done
cp -a "$assets/aic8800D80/." "$stage/lib/firmware/aic8800D80/"
install -m755 "$assets/quectel-CM" "$stage/usr/sbin/quectel-CM"
install -m755 "$br/output/target/usr/sbin/chronyd" "$stage/usr/sbin/chronyd"
install -m755 "$br/output/target/usr/bin/chronyc" "$stage/usr/bin/chronyc"
# The vendor base starts dhcpcd globally; WiFi belongs to mlk_wifi_dhcp.
# Two clients on wlan0 otherwise acquire different leases and source addresses.
touch "$stage/etc/dhcpcd.conf"
if ! grep -qx 'denyinterfaces wlan\*' "$stage/etc/dhcpcd.conf"; then
    printf '\n# WiFi DHCP is managed by mlk_wifi_dhcp.\ndenyinterfaces wlan*\n' >> "$stage/etc/dhcpcd.conf"
fi
"${cross}readelf" -h "$stage/usr/sbin/quectel-CM" | grep 'Machine:.*AArch64' >/dev/null
echo "Network components staged for kernel $release; cellular dialling is manual."
