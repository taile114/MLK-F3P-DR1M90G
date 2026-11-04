#!/usr/bin/env bash
# Assemble the on-board Qt bundle into the rootfs overlay:
#   opt/qt_play/  =  mlk_ps_local  +  qt_lib/ (5.13.2, has linuxfb)  +  run_arm_app.sh
# Then platform/flash/build_rootfs.sh packs it into rootfs.tar.gz.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
QT_SDK_ROOT="${QT_SDK_ROOT:-$REPO/host/vendor/qt/uisrc-lab-qt-aarch64/uisrc-lab-qt}"

BIN="$HERE/build-arm64/mlk_ps_local"
QTLIB_SRC="$QT_SDK_ROOT/applications/qt_play/qt_lib"
DEST="$REPO/platform/flash/rootfs-overlay/opt/qt_play"

[[ -f "$BIN" ]] || { echo "ERROR: build first (missing $BIN)" >&2; exit 1; }
[[ -d "$QTLIB_SRC" ]] || { echo "ERROR: qt_lib not found: $QTLIB_SRC" >&2; exit 1; }

mkdir -p "$DEST"
cp -f "$BIN" "$DEST/mlk_ps_local"
cp -f "$HERE/run_arm_app.sh" "$DEST/run_arm_app.sh"
if command -v rsync >/dev/null; then
  rsync -a --delete "$QTLIB_SRC/" "$DEST/qt_lib/"
else
  rm -rf "$DEST/qt_lib"; cp -a "$QTLIB_SRC" "$DEST/qt_lib"
fi
# CJK font for Qt linuxfb (run_arm_app.sh sets QT_QPA_FONTDIR to this dir).
FONTS_DEST="$DEST/qt_lib/lib/fonts"
FONTS_SRC="${MAPLE_FONTS_DIR:-$(ls -d /nix/store/*-MapleMono-NF-CN-*/share/fonts/truetype 2>/dev/null | head -1)}"
if [[ -n "${FONTS_SRC:-}" && -d "$FONTS_SRC" ]]; then
  mkdir -p "$FONTS_DEST"
  for w in Regular Medium Bold; do
    [[ -f "$FONTS_SRC/MapleMono-NF-CN-$w.ttf" ]] && cp -f "$FONTS_SRC/MapleMono-NF-CN-$w.ttf" "$FONTS_DEST/"
  done
  echo "== fonts <- $FONTS_SRC (Regular/Medium/Bold)"
fi

chmod +x "$DEST/run_arm_app.sh" "$DEST/mlk_ps_local"

echo "== bundled -> $DEST"
ls -l "$DEST"
