#!/usr/bin/env bash
# Cross-compile host/ps-local for MLK-F3P-DR1M90G (aarch64, vendor Qt 5.13.2).
#
# Route A: vendor cross Qt SDK (uisrc-lab-qt-aarch64). Two prerequisites this
# script handles/sets up:
#   1. The SDK hardcodes /home/uisrc/uisrc-lab-qt, so qmake cannot find its
#      mkspecs/features when extracted elsewhere -> a qt.conf relocates it.
#   2. PKGCONFIG += openssl needs a cross OpenSSL 1.1 -> run build_openssl_arm.sh
#      once (or point QT_OPENSSL_SYSROOT at an existing sysroot).
#
# Overrides: QT_SDK_ROOT, QT_TOOLCHAIN_BIN, QT_OPENSSL_SYSROOT
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

QT_SDK_ROOT="${QT_SDK_ROOT:-$REPO/host/vendor/qt/uisrc-lab-qt-aarch64/uisrc-lab-qt}"
QT_TOOLCHAIN_BIN="${QT_TOOLCHAIN_BIN:-$REPO/platform/flash/vendor/tools/aarch64-linux/bin}"
QT_OPENSSL_SYSROOT="${QT_OPENSSL_SYSROOT:-$REPO/host/vendor/openssl-aarch64}"

[[ -d "$QT_SDK_ROOT" ]] || { echo "ERROR: SDK not found: $QT_SDK_ROOT" >&2; exit 1; }
QTDIR="$QT_SDK_ROOT/images/qt5-arm64-5.13.2"
[[ -x "$QTDIR/bin/qmake" ]] || { echo "ERROR: cross qmake not found: $QTDIR/bin/qmake" >&2; exit 1; }
[[ -x "$QT_TOOLCHAIN_BIN/aarch64-linux-gnu-g++" ]] || { echo "ERROR: cross g++ not found: $QT_TOOLCHAIN_BIN" >&2; exit 1; }
[[ -f "$QT_OPENSSL_SYSROOT/lib/pkgconfig/openssl.pc" ]] || {
  echo "ERROR: cross OpenSSL missing; run $HERE/build_openssl_arm.sh first" >&2; exit 1; }
PKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" || {
  echo 'ERROR: host pkg-config not found in PATH' >&2; exit 1; }
BUILD="${QT_BUILD_DIR:-$HERE/build-arm64}"
mkdir -p "$BUILD/pkgconfig"
for pc in "$QT_OPENSSL_SYSROOT"/lib/pkgconfig/*.pc; do
  awk -v prefix="$QT_OPENSSL_SYSROOT" '/^prefix=/ {$0="prefix=" prefix} {print}' \
    "$pc" > "$BUILD/pkgconfig/$(basename "$pc")"
done

# 1) Relocate the SDK (its built-in prefix points at /home/uisrc/...).
if [[ ! -f "$QTDIR/bin/qt.conf" ]]; then
  printf '[Paths]\nPrefix=%s\n' "$QTDIR" > "$QTDIR/bin/qt.conf"
  echo "== wrote $QTDIR/bin/qt.conf"
fi

# 2) Use explicit SDK paths; qt_cfg.sh is optional and hardcodes its extraction path.
export QTDIR
unset QT_INSTALL_PREFIX QT_PLUGIN_PATH QML2_IMPORT_PATH QMAKEPATH QMAKEFEATURES
export QMAKESPEC="$QTDIR/mkspecs/linux-aarch64-gnu-g++"
export PATH="$QT_TOOLCHAIN_BIN:$QTDIR/bin:$PATH"
export PKG_CONFIG_PATH="$BUILD/pkgconfig"
export PKG_CONFIG_LIBDIR="$BUILD/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${PKG_CONFIG_SYSROOT_DIR:-/}"
export QMAKE_PKG_CONFIG="$PKG_CONFIG_EXECUTABLE"
unset LD_LIBRARY_PATH   # target aarch64 libs must not shadow host tools

# 3) Out-of-source build.
cd "$BUILD"
qmake "$HERE/mlk_ps_local.pro" 'QMAKE_LFLAGS_RPATH=' "QMAKE_PKG_CONFIG=$QMAKE_PKG_CONFIG"
make -j"$(nproc)"

BIN="$BUILD/mlk_ps_local"
[[ -f "$BIN" ]] || { echo "ERROR: no $BIN produced" >&2; exit 1; }
file "$BIN"
case "$(file -b "$BIN")" in
  *aarch64*) echo "== OK: $BIN" ;;
  *) echo "ERROR: $BIN is not aarch64" >&2; exit 1 ;;
esac
