#!/usr/bin/env bash
# Manage GUI-session autostart on a Linux PC with the project's Nix environment.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
unit="$unit_dir/mlk-remote-pc.service"
case "${1:-install}" in
    install)
        [[ -x "$repo/host/remote-pc/mlk_udp_host" ]] || { echo 'Build remote-pc first' >&2; exit 1; }
        systemctl --user stop mlk-remote-pc.service 2>/dev/null || true
        mkdir -p "$unit_dir"
        cat > "$unit" <<EOF
[Unit]
Description=MLK-F3P remote-pc
PartOf=graphical-session.target
After=graphical-session.target

[Service]
Type=simple
WorkingDirectory=$repo
Environment=QT_QPA_PLATFORM=xcb
ExecStart=$repo/host/remote-pc/run_host_app.sh
Restart=on-failure
RestartSec=5
UMask=0077

[Install]
WantedBy=graphical-session.target
EOF
        systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR
        systemctl --user daemon-reload
        systemctl --user enable --now mlk-remote-pc.service ;;
    remove)
        systemctl --user disable --now mlk-remote-pc.service 2>/dev/null || true
        rm -f "$unit"
        systemctl --user daemon-reload ;;
    status) systemctl --user --no-pager status mlk-remote-pc.service ;;
    *) echo 'usage: install_remote_pc.sh {install|remove|status}' >&2; exit 2 ;;
esac
