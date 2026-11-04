#!/usr/bin/env bash
# Stage sealed fault packages with an atomic current link and a retained previous link.
set -euo pipefail
flash="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$flash/../.." && pwd)"
: "${MLK_FAULT_ROOT:?Set the compiled fault runtime package directory}"
overlay="${ROOTFS_OVERLAY:-$flash/rootfs-overlay}"
python3 "$repo/host/ps-local/fault-inference/tools/verify_runtime.py" "$MLK_FAULT_ROOT" --aarch64
if [[ -n "${ARMNN_RUNTIME_ROOT:-}" ]]; then
    [[ -f "$ARMNN_RUNTIME_ROOT/lib/libarmnn.so.32.1" ]] || { echo 'Paired Arm NN library missing' >&2; exit 1; }
fi
mkdir -p "$overlay/opt/mlk_fault-packages" "$overlay/etc/default"
overlay="$(cd "$overlay" && pwd)"
stage="$(mktemp -d "$overlay/opt/mlk_fault-packages/.stage-XXXXXX")"
trap 'rmdir "$stage" 2>/dev/null || true' EXIT
python3 - "$MLK_FAULT_ROOT" "$stage" <<'PY'
import json, pathlib, shutil, sys
root, dest = map(pathlib.Path, sys.argv[1:])
manifest = json.loads((root / 'model.json').read_text())
for name in ['model.json'] + sorted(manifest['files']):
    shutil.copyfile(str(root / name), str(dest / name))
    (dest / name).chmod(0o644)
PY
python3 "$repo/host/ps-local/fault-inference/tools/verify_runtime.py" "$stage" --aarch64
version="$(python3 - "$stage/model.json" <<'PY'
import hashlib, json, pathlib, sys
p = pathlib.Path(sys.argv[1]); raw = p.read_bytes()
print(json.loads(raw.decode('utf-8'))['model'] + '-' + hashlib.sha256(raw).hexdigest()[:16])
PY
)"
store="$overlay/opt/mlk_fault-packages"
target="mlk_fault-packages/$version"
if [[ -e "$store/$version" ]]; then
    python3 "$repo/host/ps-local/fault-inference/tools/verify_runtime.py" "$store/$version" --aarch64
    # An identical sealed version already exists. Remove only this invocation's copied files.
    find "$stage" -maxdepth 1 -type f -delete
    rmdir "$stage"
else
    mv "$stage" "$store/$version"
fi
current="$overlay/opt/mlk_fault"
previous="$overlay/opt/mlk_fault.previous"
if [[ -L "$current" ]]; then
    old="$(readlink "$current")"
    if [[ "$old" != "$target" ]]; then
        ln -s "$old" "$overlay/opt/.fault-previous-$$"
        mv -Tf "$overlay/opt/.fault-previous-$$" "$previous"
    fi
elif [[ -e "$current" ]]; then
    mv "$current" "$store/legacy-$$"
    ln -s "mlk_fault-packages/legacy-$$" "$overlay/opt/.fault-previous-$$"
    mv -Tf "$overlay/opt/.fault-previous-$$" "$previous"
fi
ln -s "$target" "$overlay/opt/.fault-current-$$"
mv -Tf "$overlay/opt/.fault-current-$$" "$current"
install -m644 "$flash/rootfs-overlay/etc/default/mlk-inference" "$overlay/etc/default/mlk-inference.new"
mv -f "$overlay/etc/default/mlk-inference.new" "$overlay/etc/default/mlk-inference"
if [[ -n "${ARMNN_RUNTIME_ROOT:-}" ]]; then
    mkdir -p "$overlay/opt/armnn_lib/lib"
    cp -a "$ARMNN_RUNTIME_ROOT/lib/." "$overlay/opt/armnn_lib/lib/"
fi
echo "Staged fault package: /opt/$target; previous version retained when present"
