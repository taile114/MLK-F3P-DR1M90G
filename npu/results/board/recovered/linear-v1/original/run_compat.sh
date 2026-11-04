#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test -x "$root/compat_timeout_v1/timeout" || { echo 'Missing timeout compatibility helper' >&2; exit 2; }
export PATH="$root/compat_timeout_v1:$PATH"
exec sh "$root/run.sh" "$@"
