#!/usr/bin/env bash
# Stage a sealed inference bundle and the paired vendor ArmNN runtime.
set -euo pipefail
flash="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${RUL_BUNDLE:?Set compiled, sealed application bundle directory}"
: "${ARMNN_RUNTIME_ROOT:?Set paired ArmNN include/lib root}"
python3 - "$RUL_BUNDLE" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
m = json.loads((root / 'model.json').read_text())
required = {'weights.bin', 'weights.tsv', 'qparams_ff6.tsv', 'input_f32.bin', 'libmlk_rul.so'}
if (m.get('model_id') != 'motor_stable23_v1' or set(m.get('sha256', {})) != required
    or m.get('execution_plan') != 'ff6' or m.get('quantization_plan') != 'qparams_ff6.tsv'
    or m.get('npu_branches') != 6):
    raise SystemExit('Invalid/unsealed inference bundle')
for name in required:
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != m['sha256'][name]:
        raise SystemExit('Inference bundle checksum mismatch: ' + name)
PY
dest="$flash/rootfs-overlay/opt/mlk_rul"
mkdir -p "$dest" "$flash/rootfs-overlay/opt/armnn_lib/lib"
# The destination is reused between plans. Drop only the model payload this
# script owns, so a file from an older plan (for example qparams_head.tsv)
# cannot stay behind and contradict the sealed manifest.
rm -f "$dest"/model.json "$dest"/checksums.sha256 "$dest"/weights.bin "$dest"/weights.tsv \
      "$dest"/qparams_*.tsv "$dest"/input_f32.bin "$dest"/reference_f32.bin \
      "$dest"/labels_f32.bin "$dest"/labels.csv \
      "$dest"/libmlk_rul.so "$dest"/check_engine
for name in model.json checksums.sha256 weights.bin weights.tsv qparams_ff6.tsv input_f32.bin libmlk_rul.so; do
    install -m644 "$RUL_BUNDLE/$name" "$dest/$name"
done
# 窗口寿命标签可选：面板据此在寿命预测后显示预测偏差，缺失时只显示预测值。
if [[ -f "$RUL_BUNDLE/labels_f32.bin" ]]; then
    install -m644 "$RUL_BUNDLE/labels_f32.bin" "$dest/labels_f32.bin"
fi
if [[ -f "$RUL_BUNDLE/labels.csv" ]]; then
    install -m644 "$RUL_BUNDLE/labels.csv" "$dest/labels.csv"
fi
[[ -f "$ARMNN_RUNTIME_ROOT/lib/libarmnn.so.32.1" ]] || { echo 'Paired ArmNN library missing' >&2; exit 1; }
cp -a "$ARMNN_RUNTIME_ROOT/lib/." "$flash/rootfs-overlay/opt/armnn_lib/lib/"
# The board acceptance procedure runs check_engine from /opt/mlk_rul-validation.
# Stage it only when the bundle carries the checker and its reference series;
# the model data stays in /opt/mlk_rul and is reached through relative links.
validation="$flash/rootfs-overlay/opt/mlk_rul-validation"
if [[ -f "$RUL_BUNDLE/check_engine" && -f "$RUL_BUNDLE/reference_f32.bin" ]]; then
    mkdir -p "$validation"
    install -m755 "$RUL_BUNDLE/check_engine" "$validation/check_engine"
    install -m755 "$flash/../../host/ps-local/inference/inspect_environment.sh" "$validation/inspect_environment.sh"
    install -m644 "$RUL_BUNDLE/reference_f32.bin" "$validation/reference_f32.bin"
    ln -sfn ../mlk_rul/libmlk_rul.so "$validation/libmlk_rul.so"
    ln -sfn ../mlk_rul/qparams_ff6.tsv "$validation/qparams_ff6.tsv"
    ln -sfn ../mlk_rul/input_f32.bin "$validation/input_f32.bin"
    # The engine loads weights.bin and weights.tsv relative to the root it is
    # given, so the checker root needs both or create() fails with "No weights
    # loaded" before any window runs.
    ln -sfn ../mlk_rul/weights.bin "$validation/weights.bin"
    ln -sfn ../mlk_rul/weights.tsv "$validation/weights.tsv"
fi
echo "Staged model bundle in $dest; verify NPU kernel/DTB on target separately"
