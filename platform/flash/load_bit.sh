#!/usr/bin/env bash
# Build a device-tree overlay that loads one PL bitstream through of-fpga-region,
# and optionally push it to a board over a serial console FIFO.
#
# The vendor 6.1.111-rt42 kernel exposes only name/state/status on fpga_manager
# (no firmware attribute), so the bitstream is selected with an overlay that sets
# firmware-name on /soc/base_fpga_region. The file is read from /lib/firmware.
#
# Usage:
#   load_bit.sh soc_hdmi.bit                     # build /tmp/load_bit/soc_hdmi.dtbo
#   load_bit.sh board_test.bit --send /tmp/board_console/ttyACM0.in
#   load_bit.sh /path/to/x.bit --region /soc/base_fpga_region --out /tmp/o
set -euo pipefail
FLASH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$FLASH_DIR/../.." && pwd)"
FW_DIR="$FLASH_DIR/rootfs-overlay/lib/firmware"
REGION="/soc/base_fpga_region"
OUT="$REPO/build/load_bit"
SEND=""

usage() { sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

BIT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --send) SEND="$2"; shift 2 ;;
        --region) REGION="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
        *) BIT="$1"; shift ;;
    esac
done
[[ -n "$BIT" ]] || { usage >&2; exit 2; }

if [[ "$BIT" == */* ]]; then
    SRC="$BIT"
else
    [[ "$BIT" == *.bit ]] || BIT="$BIT.bit"
    SRC="$FW_DIR/$BIT"
fi
[[ -f "$SRC" ]] || { echo "bitstream not found: $SRC" >&2; exit 1; }

NAME="$(basename "$SRC")"
# TD writes an ASCII bitstream whose first line is a banner; the kernel manager
# strips it. Reject binary .bit files here instead of failing on the board.
head -n 1 "$SRC" | grep -q "Anlogic ASCII Bitstream" || {
    echo "not a TD ASCII bitstream (missing banner): $SRC" >&2
    echo "generate it with bitgen in TD; the FPGA manager needs the ASCII form" >&2
    exit 1
}

command -v dtc >/dev/null 2>&1 || {
    echo "dtc not found; run inside 'nix shell nixpkgs#dtc' (or 'nix develop')" >&2
    exit 1
}

mkdir -p "$OUT"
DTS="$OUT/$NAME.dts"
DTBO="$OUT/$NAME.dtbo"
cat > "$DTS" <<EOF
/dts-v1/;
/plugin/;

/ {
	fragment@0 {
		target-path = "$REGION";
		__overlay__ {
			firmware-name = "$NAME";
		};
	};
};
EOF
dtc -I dts -O dtb -o "$DTBO" "$DTS" 2>/dev/null

echo "== overlay: $DTBO ($(stat -c%s "$DTBO") B, sha256 $(sha256sum "$DTBO" | cut -c1-16))"
echo "== bitstream: $NAME, sha256 $(sha256sum "$SRC" | cut -c1-16)"

B64="$(base64 -w0 "$DTBO")"
if [[ -n "$SEND" ]]; then
    [[ -p "$SEND" ]] || { echo "not a fifo: $SEND" >&2; exit 1; }
    # One line per shell command keeps the serial console readable.
    printf 'echo %s > /tmp/pl.dtbo.b64\n' "'$B64'" > "$SEND"
    printf 'mount -t configfs none /sys/kernel/config 2>/dev/null; mkdir -p /sys/kernel/config/device-tree/overlays/pl; base64 -d /tmp/pl.dtbo.b64 > /sys/kernel/config/device-tree/overlays/pl/dtbo; sleep 1; cat /sys/class/fpga_manager/fpga0/state; dmesg | tail -2\n' > "$SEND"
    echo "== sent to $SEND"
else
    cat <<EOF
Send to the board (the dtbo must land on the board's filesystem):

  base64 -w0 $DTBO        # copy the single line to the board
  # on the board:
  echo '<base64>' > /tmp/pl.dtbo.b64
  base64 -d /tmp/pl.dtbo.b64 > /tmp/pl.dtbo
  mount -t configfs none /sys/kernel/config
  mkdir -p /sys/kernel/config/device-tree/overlays/pl
  cat /tmp/pl.dtbo > /sys/kernel/config/device-tree/overlays/pl/dtbo
  cat /sys/class/fpga_manager/fpga0/state     # expect: operating

Or pass --send <fifo> to push the same steps through a serial console FIFO
when the board has no network.

The bitstream must exist on the board as /lib/firmware/$NAME; stage_ps_local.sh
copies soc_hdmi.bit and board_test.bit there. Loading survives only until power
off; FSBL reloads the bitstream inside BOOT.bin on the next boot.
EOF
fi
