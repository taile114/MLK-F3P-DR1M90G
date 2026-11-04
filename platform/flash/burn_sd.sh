#!/usr/bin/env bash
# Burn the SD card for MLK-F3P-DR1M90G.
# Format the filesystems at fixed offsets FIRST (no partition table yet, so the
# desktop cannot automount anything), THEN write the MBR. This avoids the
# automount race that let a stale mount clobber a freshly written filesystem.
# Usage: sudo env PATH="$PATH" bash burn_sd.sh /dev/sdX
set -euo pipefail
FLASH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR="$FLASH_DIR/vendor"
IMAGES="${IMAGES_ROOT:-$VENDOR/boards/DR1M/buildroot/images}"
DEV="${1:?Pass the explicitly identified SD device, for example /dev/sdX}"

[[ "$(id -u)" -eq 0 ]] || { echo "ERROR: run as root (sudo)" >&2; exit 1; }
[[ -b "$DEV" ]] || { echo "ERROR: not a block device: $DEV" >&2; exit 1; }
[[ -f "$IMAGES/boot/BOOT.bin" && -f "$IMAGES/rootfs/rootfs.tar.gz" ]] || { echo "ERROR: images missing under $IMAGES" >&2; exit 1; }
# partprobe is optional on NixOS (not shipped by util-linux); the calls below
# already tolerate its absence. mkfs.ext4 / e2fsck come from e2fsprogs.
for t in sfdisk losetup mkfs.vfat fsck.vfat mkfs.ext4 e2fsck partx wipefs rsync tar; do
  command -v "$t" >/dev/null || { echo "ERROR: missing tool: $t" >&2; exit 1; }
done
ROOTSRC="$(findmnt -no SOURCE / || true)"
[[ -n "$ROOTSRC" && "$ROOTSRC" == "$DEV"* ]] && { echo "ERROR: refusing to wipe $DEV (hosts / = $ROOTSRC)" >&2; exit 1; }

echo "== target =="; lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT,MODEL "$DEV"

echo "== unmount + drop partition table (no kernel partitions during format) =="
for mp in $(lsblk -no MOUNTPOINT "$DEV" | sed '/^$/d'); do umount "$mp" 2>/dev/null || umount -l "$mp" 2>/dev/null || true; done
partx -d "$DEV" 2>/dev/null || true
wipefs -a "$DEV" || true
blockdev --rereadpt "$DEV" 2>/dev/null || true
udevadm settle 2>/dev/null || true
sleep 1
lsblk "$DEV"

P1_OFF=$((2048*512)); P1_LEN=$((202752*512)); P2_OFF=$((204800*512))
L1=""; L2=""; MNT=""
cleanup(){ [[ -n "$MNT" ]] && umount "$MNT" 2>/dev/null || true; [[ -n "$L1" ]] && losetup -d "$L1" 2>/dev/null || true; [[ -n "$L2" ]] && losetup -d "$L2" 2>/dev/null || true; }
trap cleanup EXIT

echo "== losetup + format (offsets: p1=$P1_OFF p2=$P2_OFF) =="
L1="$(losetup -f --show -o "$P1_OFF" --sizelimit "$P1_LEN" "$DEV")"
L2="$(losetup -f --show -o "$P2_OFF" "$DEV")"
mkfs.vfat -F 32 -n BOOT "$L1"
mkfs.ext4 -F -L rootfs "$L2"

MNT="$(mktemp -d)"
echo "== boot -> FAT =="
mount -t vfat "$L1" "$MNT"
rsync -r --no-perms --no-owner --no-group --modify-window=1 "$IMAGES/boot/" "$MNT/"
sync; umount "$MNT"
echo "== rootfs -> ext4 =="
mount -t ext4 "$L2" "$MNT"
tar zxmf "$IMAGES/rootfs/rootfs.tar.gz" -C "$MNT"
sync; umount "$MNT"

echo "== verify (loops, before exposing table) =="
e2fsck -fn "$L2"
fsck.vfat -n "$L1" | tail -n 2
losetup -d "$L1"; losetup -d "$L2"; L1=""; L2=""

echo "== write MBR (now partitions appear; kernel reads the NEW fs) =="
sfdisk --force --quiet "$DEV" <<'EOF'
label: dos
unit: sectors
start=2048, size=202752, type=0c, bootable
start=204800, type=83
EOF
partprobe "$DEV" 2>/dev/null || true
partx -u "$DEV" 2>/dev/null || true
blockdev --rereadpt "$DEV" 2>/dev/null || true
udevadm settle 2>/dev/null || true
sleep 1

echo "== unmount any automounts of the new fs =="
for mp in $(lsblk -no MOUNTPOINT "$DEV" | sed '/^$/d'); do umount "$mp" 2>/dev/null || umount -l "$mp" 2>/dev/null || true; done
sync
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$DEV"
echo "== DONE: $DEV =="
