#!/usr/bin/env bash
# Build the project rootfs = vendor base rootfs + locally built ncurses/vim.
# This is the targeted workflow used by this project (NOT a from-scratch full
# Buildroot build; cfg_rootfs.sh would `distclean` and make_rootfs.sh builds all packages).
set -euo pipefail
FLASH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR="$FLASH_DIR/vendor"
BR="$VENDOR/sources/buildroot"
BASE="$VENDOR/boards/DR1M/buildroot/output/rootfs.tar.gz"
OUT="$VENDOR/boards/DR1M/buildroot/images/rootfs/rootfs.tar.gz"

[[ -f "$BASE" ]] || { echo "ERROR: vendor base rootfs missing: $BASE" >&2; exit 1; }

"$FLASH_DIR/apply_sdk_local.sh"

if ! grep -q '^BR2_PACKAGE_VIM=y' "$BR/.config" 2>/dev/null; then
  cp "$VENDOR/boards/DR1M/configs/defconfigs/anlogic_dr1_buildroot_defconfig" "$BR/configs/anlogic_dr1_buildroot_defconfig"
  make -C "$BR" anlogic_dr1_buildroot_defconfig
fi

make -C "$BR" vim
make -C "$BR" pppd

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
tar xzf "$BASE" -C "$STAGE"
mkdir -p "$STAGE/usr/bin" "$STAGE/usr/lib" "$STAGE/usr/share"
cp -a "$BR/output/target/usr/bin/vim" "$BR/output/target/usr/bin/vimdiff" "$STAGE/usr/bin/"
cp -a "$BR/output/target/usr/lib/"libncurses.so* "$BR/output/target/usr/lib/libcurses.so" "$STAGE/usr/lib/"
cp -a "$BR/output/target/usr/share/vim" "$STAGE/usr/share/"
cp -a "$BR/output/target/usr/share/terminfo" "$STAGE/usr/share/"

# PPPoE：校园网拨号需要 pppd、chat、pppoe 插件与 pppoe-discovery。
install -d "$STAGE/usr/sbin" "$STAGE/usr/lib/pppd" "$STAGE/etc/ppp/peers"
for tool in pppd chat pppdump pppstats pppoe-discovery; do
  [[ -f "$BR/output/target/usr/sbin/$tool" ]] || { echo "ERROR: pppd tool missing: $tool" >&2; exit 1; }
  cp -a "$BR/output/target/usr/sbin/$tool" "$STAGE/usr/sbin/"
done
cp -a "$BR/output/target/usr/lib/pppd/." "$STAGE/usr/lib/pppd/"
chmod 700 "$STAGE/etc/ppp"

"$FLASH_DIR/stage_network.sh" "$STAGE"

OVERLAY="$FLASH_DIR/rootfs-overlay"
if [[ -d "$OVERLAY" ]]; then
  echo "== overlay: $OVERLAY =="
  ( cd "$OVERLAY" && tar cf - --exclude=.gitignore --exclude=.gitkeep . ) | tar xf - -C "$STAGE"
fi

python3 "$FLASH_DIR/verify_npu_rootfs.py" "$STAGE"
( cd "$STAGE" && tar czf "$OUT" --owner=0 --group=0 --numeric-owner . )

echo "== rootfs -> $OUT =="
ls -l "$OUT"
