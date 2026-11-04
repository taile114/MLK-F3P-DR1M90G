#!/usr/bin/env bash
# Run the built PC application with the repository's matching Qt environment.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[[ -x "$here/mlk_udp_host" ]] || { echo 'Build host/remote-pc/mlk_udp_host first' >&2; exit 1; }
exec nix develop "$here/.." --command "$here/mlk_udp_host" "$@"
