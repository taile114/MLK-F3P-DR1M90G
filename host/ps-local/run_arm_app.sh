#!/bin/sh
# On-board launcher: run the PS-local Qt HMI on HDMI via linuxfb.
# Deployed to /opt/qt_play/ together with the binary and qt_lib/.
QTDIR="$(cd "$(dirname "$0")" && pwd)"
QTLIB="$QTDIR/qt_lib"

# BusyBox init uses HOME=/ while SSH uses /root. Keep one persistent config.
umask 077
export XDG_CONFIG_HOME="${MLK_CONFIG_HOME:-/var/lib/mlk/config}"
export TZ="${TZ:-CST-8}"
mkdir -p "$XDG_CONFIG_HOME/MLK-F3P" || exit 1
config="$XDG_CONFIG_HOME/MLK-F3P/mlk_ps_local.conf"
if [ ! -f "$config" ]; then
    for previous in /root/.config/MLK-F3P/mlk_ps_local.conf /.config/MLK-F3P/mlk_ps_local.conf; do
        [ -f "$previous" ] || continue
        cp -p "$previous" "$config" || exit 1
        chmod 600 "$config"
        break
    done
fi

export QT_PLUGIN_PATH="$QTLIB/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$QTLIB/plugins/platforms"
export QT_QPA_FONTDIR="$QTLIB/lib/fonts"
export LD_LIBRARY_PATH="$QTLIB/lib:/opt/armnn_lib/lib:${LD_LIBRARY_PATH:-}"
# 故障运行包与模型模式：环境值覆盖保存值，默认关闭且不改动 RUL 配置。
inference_config="${MLK_INFERENCE_CONFIG:-/etc/default/mlk-inference}"
if [ -f "$inference_config" ]; then
    . "$inference_config" || exit 1
fi
export MLK_FAULT_ROOT="${MLK_FAULT_ROOT:-/opt/mlk_fault}"
export MLK_FAULT_EVAL_ROOT="${MLK_FAULT_EVAL_ROOT:-/data/fault-eval}"
export MLK_INFERENCE_MODE="${MLK_INFERENCE_MODE:-off}"
# USB HID 鼠标/键盘：linuxfb 不会自动加载 evdev 输入插件。
export QT_QPA_GENERIC_PLUGINS="evdevmouse,evdevkeyboard"
export QT_QPA_PLATFORM="linuxfb:fb=/dev/fb0"

exec "$QTDIR/mlk_ps_local" --kiosk
