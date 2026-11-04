#!/usr/bin/env bash
# 安装 / 卸载板卡时间周期同步的 systemd 用户服务。
#
# 板卡没有 RTC，掉电后从 rootfs 的时间戳起算；串口连上后由主机把当前时间推给
# 板卡（tools/sync_board_time.sh --interval N）。本脚本把那条命令装成一个常驻
# 用户服务：插上串口即自动同步，板卡重启或重新枚举后自动重新登录。
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
script=$repo/tools/sync_board_time.sh
unit_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
unit=$unit_dir/mlk-board-timesync.service
device=${BOARD_TTY:-/dev/ttyACM0}
interval=${INTERVAL:-30}

usage() { echo "usage: $0 [install|remove|status]" >&2; exit 2; }

case "${1:-install}" in
install)
    [ -x "$script" ] || { echo "missing $script" >&2; exit 1; }
    mkdir -p "$unit_dir"
    cat >"$unit" <<EOF
[Unit]
Description=MLK-F3P board clock sync over the debug UART

[Service]
Type=simple
Environment=BOARD_TTY=$device BOARD_USER=root BOARD_PASSWORD=root
Environment=PYTHONUNBUFFERED=1
ExecStart=$script --interval $interval
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable --now mlk-board-timesync.service
    echo "installed $unit (device $device, every ${interval}s)"
    ;;
remove)
    systemctl --user disable --now mlk-board-timesync.service 2>/dev/null || true
    rm -f "$unit"
    systemctl --user daemon-reload
    echo "removed $unit"
    ;;
status)
    systemctl --user --no-pager status mlk-board-timesync.service
    ;;
*)
    usage
    ;;
esac
