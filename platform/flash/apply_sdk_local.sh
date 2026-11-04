#!/usr/bin/env bash
# Apply local (non-vendor) patches to the extracted SDK under vendor/, idempotently.
set -euo pipefail
FLASH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR="$FLASH_DIR/vendor"

[[ -d "$VENDOR" ]] || { echo "ERROR: $VENDOR not found" >&2; exit 1; }

BASH_BIN="$(command -v bash)"

echo "== 1) fix /bin/bash shebangs (NixOS) =="
for d in "$VENDOR/scripts" "$VENDOR/tools/bootgen"; do
  [[ -d "$d" ]] || continue
  while IFS= read -r f; do
    sed -i "1s|^#!/bin/bash|#!$BASH_BIN|" "$f"
  done < <(grep -rl '^#!/bin/bash' "$d" 2>/dev/null || true)
done

echo "== 2) buildroot 'file' dependency check (NixOS) =="
DEP="$VENDOR/sources/buildroot/support/dependencies/dependencies.sh"
if [[ -f "$DEP" ]]; then
  sed -i 's|check_prog_host "/usr/bin/file"|check_prog_host "file"|' "$DEP"
fi

echo "== 3) enable vim in buildroot defconfig =="
DEF="$VENDOR/boards/DR1M/configs/defconfigs/anlogic_dr1_buildroot_defconfig"
if [[ -f "$DEF" ]]; then
  if ! grep -q '^BR2_PACKAGE_VIM=y' "$DEF"; then
    printf 'BR2_PACKAGE_BUSYBOX_SHOW_OTHERS=y\nBR2_PACKAGE_VIM=y\nBR2_PACKAGE_VIM_RUNTIME=y\n' >> "$DEF"
  fi
fi

echo "done: local patches applied to vendor/"

# Autoreconf helper commands must resolve through PATH on NixOS.
for makefile in "$VENDOR/sources/buildroot/package/autoconf/autoconf.mk" \
                "$VENDOR/sources/buildroot/package/Makefile.in"; do
  [[ -f "$makefile" ]] && sed -i 's|/bin/true|true|g' "$makefile"
done
