#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
qt="${QT_CROSS_ROOT:-$root/host/vendor/qt/uisrc-lab-qt-aarch64/uisrc-lab-qt/images/qt5-arm64-5.13.2}"
toolchain="${CROSS_BIN:-$root/platform/flash/vendor/tools/aarch64-linux/bin}"
openssl="${QT_OPENSSL_SYSROOT:-$root/host/vendor/openssl-aarch64}"

export PATH="$qt/bin:$toolchain:$PATH"

[[ -x "$qt/bin/qmake" && -x "$qt/bin/moc" ]] || {
    echo "ERROR: cross Qt missing: $qt" >&2; exit 1;
}
command -v aarch64-linux-gnu-g++ >/dev/null
[[ -f "$openssl/lib/pkgconfig/openssl.pc" ]] || {
    echo "ERROR: cross OpenSSL missing; run $root/host/ps-local/build_openssl_arm.sh" >&2;
    exit 1;
}
if [[ ! -f "$qt/bin/qt.conf" ]]; then
    printf '[Paths]\nPrefix=%s\n' "$qt" > "$qt/bin/qt.conf"
fi

export PKG_CONFIG_PATH="$openssl/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$openssl/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${PKG_CONFIG_SYSROOT_DIR:-/}"
# Native Qt development shells set variables that override a cross qmake's prefix.
unset QT_INSTALL_PREFIX QT_PLUGIN_PATH QML2_IMPORT_PATH QMAKEPATH QMAKEFEATURES
export QMAKESPEC="$qt/mkspecs/linux-aarch64-gnu-g++"
export QMAKE_PKG_CONFIG="$(command -v pkg-config)"
unset LD_LIBRARY_PATH || true

mkdir -p "$root/host/ps-local/build-arm"
cd "$root/host/ps-local/build-arm"
"$qt/bin/qmake" ../mlk_ps_local.pro 'QMAKE_LFLAGS_RPATH=' "QMAKE_PKG_CONFIG=$QMAKE_PKG_CONFIG"
make -j"${JOBS:-4}"
aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -Wextra -Werror -I../src ../tools/plctl.cpp ../src/pl_host.cpp -o plctl
file mlk_ps_local plctl

aarch64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -I../../common ../tools/mlk-fault.c ../../common/mlk_fault_client.c -o mlk-fault

mkdir -p capture-tool
cd capture-tool
"$qt/bin/qmake" ../../../common/capturetool.pro 'QMAKE_LFLAGS_RPATH=' "QMAKE_PKG_CONFIG=$QMAKE_PKG_CONFIG"
make -j"${JOBS:-4}"
file mlk-capture

# An application-only package preserves the board's pairing and acquisition data.
tar --owner=0 --group=0 --numeric-owner \
    --transform='s|^mlk_ps_local$|opt/qt_play/mlk_ps_local|' \
    -C "$root/host/ps-local/build-arm" \
    -cf "$root/host/ps-local/build-arm/ps-local-app-update.tar" mlk_ps_local
gzip -c "$root/host/ps-local/build-arm/ps-local-app-update.tar" \
    > "$root/host/ps-local/build-arm/ps-local-app-update.tar.gz"
(
    cd "$root/host/ps-local/build-arm"
    sha256sum ps-local-app-update.tar > ps-local-app-update.tar.sha256
    sha256sum ps-local-app-update.tar.gz > ps-local-app-update.tar.gz.sha256
)
