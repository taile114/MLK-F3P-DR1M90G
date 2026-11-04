#!/usr/bin/env bash
# Copy host-side vendor resources from the resource pack into host/vendor/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/docs/MLK-F3P-DR1M90G-Resource"
dst="$root/host/vendor"

qt="$src/01_start/02_start_Linux/05_application_demo/05-1_QT环境搭建及demo演示"
udp="$src/03_demo/3-0_book_all"

mkdir -p "$dst/qt/driver" "$dst/udp"
cp "$qt/05-1_米联客-安路F3P-CZ02开发板QT环境搭建及demo演示.pdf" "$dst/qt/"
cp "$qt/demo/QT/qt_play.tar.gz" "$dst/qt/"
cp "$qt/demo/driver/"* "$dst/qt/driver/"
cp "$udp/3_1_02_米联客-安路 FPGA UDP以太网通信方案.pdf" "$dst/udp/"

echo "vendor resources copied to $dst"
