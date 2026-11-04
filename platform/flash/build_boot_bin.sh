#!/usr/bin/env bash
# Assemble BOOT.bin = FSBL + PL bitstream + U-Boot, then stage boot files.
set -euo pipefail
flash="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$flash/../.." && pwd)"
out="$flash/vendor/boards/DR1M/buildroot/output"
images="$flash/vendor/boards/DR1M/buildroot/images/boot"
albg="$flash/vendor/tools/bootgen/linux64/bin/albg"
bit="$repo/fpga/uisrc/out/soc_hdmi/soc_hdmi.bit"
# The HDMI design streams the framebuffer through PS HP0. The candidate FSBL
# was built from an HPF with Slave_AXI_HP0_Interface=0; it stalls the VDMA and
# leaves HDMI in reset. Use the vendor HDMI FSBL (HP0/1 enabled) unless the
# caller provides an explicit FSBL path.
vendor_fsbl="$repo/docs/MLK-F3P-DR1M90G-Resource/01_start/02_start_Linux/05_application_demo/05-1_QT环境搭建及demo演示/demo/soc_sdk/soc_fsbl/build/soc_fsbl.elf"
fsbl="${FSBL:-$vendor_fsbl}"

[[ -f "$bit" ]] || { echo "ERROR: HDMI bitstream missing: $bit" >&2; exit 1; }
[[ -f "$fsbl" ]] || { echo "ERROR: FSBL missing: $fsbl" >&2; exit 1; }
python3 "$repo/fpga/soc/verify_npu_bit.py"

# The merged image must include the sensor system. A vendor/DI regeneration can
# replace the wrapper with a stub that ties O_led low (all active-low LEDs on)
# and disconnects HDMI/sensor logic; refuse to package that image.
wrapper="$repo/fpga/uisrc/out/soc_hdmi/system/hdl/system_wrapper.v"
system_v="$repo/fpga/uisrc/out/soc_hdmi/system/hdl/system.v"
[[ -f "$wrapper" && -f "$system_v" ]] || {
    echo "ERROR: merged HDL missing; run fpga/soc/prepare_hdmi_soc.py first" >&2; exit 1;
}
grep -q '\.O_led(O_led)' "$wrapper" || {
    echo "ERROR: system_wrapper.v does not connect O_led; re-run fpga/soc/prepare_hdmi_soc.py" >&2; exit 1;
}
grep -q 'soc_ctrl_port X_sensor' "$system_v" || {
    echo "ERROR: system.v lacks soc_ctrl_port X_sensor; re-run fpga/soc/prepare_hdmi_soc.py" >&2; exit 1;
}
[[ -x "$albg" ]] || { echo "ERROR: albg missing: $albg" >&2; exit 1; }

cp "$fsbl" "$out/fsbl.elf"
cp "$bit" "$out/system.bit"
"$albg" -w -log info -image "$out/boot.bif" -arch dr1m90 -o "$out/BOOT.bin"
cp "$out/BOOT.bin" "$images/BOOT.bin"
cp "$out/boot.scr" "$images/boot.scr" 2>/dev/null || true
cp "$out/uImage.lz4" "$images/uImage.lz4" 2>/dev/null || true

echo "BOOT.bin -> $images/BOOT.bin"
ls -l "$images/BOOT.bin"
sha256sum "$images/BOOT.bin" "$out/system.bit" "$out/fsbl.elf"
