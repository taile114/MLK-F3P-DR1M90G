#!/usr/bin/env bash
# Cross-build OpenSSL 1.1 for aarch64, matching the board rootfs (libcrypto.so.1.1).
#
# The vendor Qt SDK does NOT ship a cross OpenSSL, and host/ps-local needs it
# (PKGCONFIG += openssl). Source comes from the vendor SDK's own buildroot dl.
#
# Output sysroot: host/vendor/openssl-aarch64  (headers + aarch64 libs + *.pc)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

OPENSSL_TAR="${OPENSSL_TAR:-$REPO/platform/baremetal-sdk/sdk/buildroot/dl/libopenssl/openssl-1.1.1q.tar.gz}"
QT_TOOLCHAIN_BIN="${QT_TOOLCHAIN_BIN:-$REPO/platform/flash/vendor/tools/aarch64-linux/bin}"
OUT="${OPENSSL_OUT:-$REPO/host/vendor/openssl-aarch64}"
WORK="${OPENSSL_WORK:-/tmp/ossl-aarch64}"

[[ -f "$OPENSSL_TAR" ]] || { echo "ERROR: OpenSSL source not found: $OPENSSL_TAR" >&2; exit 1; }
[[ -x "$QT_TOOLCHAIN_BIN/aarch64-linux-gnu-gcc" ]] || { echo "ERROR: cross gcc not found: $QT_TOOLCHAIN_BIN" >&2; exit 1; }

rm -rf "$WORK" "$OUT"; mkdir -p "$WORK"
tar xzf "$OPENSSL_TAR" -C "$WORK"
SRC="$(find "$WORK" -maxdepth 1 -type d -name 'openssl-*' | head -1)"
[[ -n "$SRC" ]] || { echo "ERROR: extraction failed" >&2; exit 1; }

cd "$SRC"
PATH="$QT_TOOLCHAIN_BIN:$PATH" ./Configure linux-aarch64 shared no-tests no-async \
    --cross-compile-prefix=aarch64-linux-gnu- \
    --prefix="$OUT" --openssldir="$OUT/ssl"
PATH="$QT_TOOLCHAIN_BIN:$PATH" make -j"$(nproc)"
PATH="$QT_TOOLCHAIN_BIN:$PATH" make install_sw

file "$OUT/lib/libcrypto.so.1.1"
echo "== OpenSSL aarch64 -> $OUT"
