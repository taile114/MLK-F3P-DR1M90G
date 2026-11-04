#!/usr/bin/env bash
set -euo pipefail

SRC="$HOME/Documents/LCEDA-Pro/projects/MLK-F3P.eprj2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DST="$ROOT/eda/MLK-F3P.eprj2"

if [ ! -f "$SRC" ]; then
  echo "not found: $SRC" >&2
  exit 1
fi

mkdir -p "$(dirname "$DST")"
cp "$SRC" "$DST"
cd "$ROOT"
git add eda/MLK-F3P.eprj2

if git diff --cached --quiet -- eda/MLK-F3P.eprj2; then
  echo "lceda: no change"
  exit 0
fi

MSG="${1:-lceda: snapshot $(date +%F_%H:%M)}"
git -c user.name="${GIT_AUTHOR_NAME:-lfour}" \
    -c user.email="${GIT_AUTHOR_EMAIL:-lfour@localhost}" \
    commit -m "$MSG" -- eda/MLK-F3P.eprj2
echo "lceda: committed -> $MSG"
