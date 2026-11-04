"""Create parser-only controls without changing trained or exported artifacts."""
from __future__ import annotations

import copy
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper, shape_inference
import onnxruntime as ort

NPU_ROOT = Path(__file__).resolve().parents[1]
MODEL_ROOT = NPU_ROOT / "training"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, default=NPU_ROOT / "work/f3p-parser-diagnostics-v1")
ROOT = parser.parse_args().output.resolve()
SOURCE = MODEL_ROOT / "outputs/motor_stable23_v1_onnx/rul_fp32.onnx"
ARCHIVE = ROOT.with_suffix(".tar.gz")
if ROOT.exists() or ARCHIVE.exists():
    raise SystemExit("Refusing to overwrite an existing diagnostics package")
ROOT.mkdir()
MODEL_DIR = ROOT / "models"
MODEL_DIR.mkdir()

original = onnx.load(SOURCE)
inferred = shape_inference.infer_shapes(original, strict_mode=True, data_prop=True)
infos = {v.name: v for v in [*inferred.graph.input, *inferred.graph.value_info, *inferred.graph.output]}
saved = []


def save(name: str, model: onnx.ModelProto) -> None:
    onnx.checker.check_model(model, full_check=True)
    onnx.save(model, MODEL_DIR / name)
    saved.append(name)


def typed_int64(model: onnx.ModelProto) -> onnx.ModelProto:
    model = copy.deepcopy(model)
    for tensor in model.graph.initializer:
        if tensor.data_type == TensorProto.INT64:
            values = numpy_helper.to_array(tensor).reshape(-1).tolist()
            tensor.ClearField("raw_data")
            tensor.ClearField("int64_data")
            tensor.int64_data.extend(values)
    return model


relu = helper.make_model(helper.make_graph(
    [helper.make_node("Relu", ["x"], ["y"], name="control_relu")],
    "control_relu", [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 23, 110])],
    [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 23, 110])]),
    opset_imports=[helper.make_opsetid("", 13)], ir_version=7)
save("00_relu.onnx", relu)

# Prefixes retain exactly the original node attributes, tensor names and constants.
for count, title in [(1, "transpose"), (2, "reshape"), (3, "unsqueeze"), (4, "less")]:
    nodes = list(original.graph.node[:count])
    used = {name for node in nodes for name in node.input}
    constants = [t for t in original.graph.initializer if t.name in used]
    graph = helper.make_graph(nodes, title, list(original.graph.input),
                              [infos[nodes[-1].output[0]]], initializer=constants)
    model = helper.make_model(graph, opset_imports=list(original.opset_import), ir_version=original.ir_version)
    save(f"{count:02d}_{title}_raw.onnx", model)
    if count >= 2:
        save(f"{count:02d}_{title}_typed_int64.onnx", typed_int64(model))

save("10_full_shapes.onnx", inferred)
save("11_full_shapes_typed_int64.onnx", typed_int64(inferred))

# Verify every variant with ONNX Runtime; full variants replay all 165 baseline windows.
options = ort.SessionOptions()
options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
options.intra_op_num_threads = 1
options.inter_op_num_threads = 1
data_dir = MODEL_ROOT / "processed/run_to_failure_5motors_v1/board_replay"
windows = np.fromfile(data_dir / "test_input_f32.bin", dtype="<f4").reshape(-1, 23, 110)
with np.load(MODEL_ROOT / "outputs/motor_stable23_v1_onnx/predictions.npz") as predictions:
    reference = predictions["pytorch_cpu"].reshape(-1)
report = {"source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(), "models": []}
for name in saved:
    session = ort.InferenceSession(str(MODEL_DIR / name), sess_options=options, providers=["CPUExecutionProvider"])
    sample = windows[:1] if name != "00_relu.onnx" else np.linspace(-1, 1, 2530, dtype=np.float32).reshape(1, 23, 110)
    result = session.run(None, {session.get_inputs()[0].name: sample})[0]
    row = {"name": name, "checker": "passed", "ort": "passed", "output_shape": list(result.shape)}
    if name.startswith(("10_", "11_")):
        predictions = np.asarray([session.run(None, {session.get_inputs()[0].name: x[None]})[0].item() for x in windows])
        errors = np.abs(predictions - reference)
        if not np.all(errors <= 1e-4 + 1e-4 * np.abs(reference)):
            raise RuntimeError(f"Reference mismatch: {name}")
        row.update(samples=len(windows), max_reference_error=float(errors.max()))
    report["models"].append(row)

(ROOT / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
script = '''#!/bin/sh
set -u
ulimit -c 0 2>/dev/null || true
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
probe=${1:-/root/f3p-rul-probe}
export LD_LIBRARY_PATH="$probe/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ ! -x "$probe/bin/rul_probe" ]; then echo "Missing executable: $probe/bin/rul_probe"; exit 2; fi
results="$here/results_$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "$results" || exit 2
if command -v ldd >/dev/null 2>&1; then ldd "$probe/bin/rul_probe" > "$results/ldd.txt" 2>&1 || true; fi
echo "results=$results"
for model in "$here"/models/*.onnx; do
    name=$(basename "$model" .onnx)
    echo "--- $name ---"
    status=0
    "$probe/bin/rul_probe" --model "$model" --parse-only > "$results/$name.log" 2>&1 || status=$?
    cat "$results/$name.log"
    echo "exit_code=$status"
    printf '%s %s\\n' "$name" "$status" >> "$results/summary.txt"
done
echo "--- summary ---"
cat "$results/summary.txt"
echo "results=$results"
'''
(ROOT / "run_diagnostics.sh").write_text(script, encoding="utf-8", newline="\n")
(ROOT / "README.txt").write_text(
    "Parser-only diagnostic controls. Requires the existing /root/f3p-rul-probe binary and libraries.\n"
    "Extract this archive on the board Linux filesystem. Run: sh run_diagnostics.sh\n"
    "00: one Relu; 01-04: original model prefixes through Transpose, Reshape, Unsqueeze and Less.\n"
    "Raw/typed pairs differ only in INT64 initializer encoding, retaining identical integer values.\n"
    "10: full original model with inferred tensor metadata.\n"
    "11: full model with inferred metadata and typed INT64 initializers.\n"
    "No quantization, retraining, NPU inference or system-library replacement is performed.\n"
    "Full variants passed all 165 PyTorch reference windows on PC ONNX Runtime.\n"
    "Results from the vendor parser on F3P are not yet available.\n", encoding="utf-8", newline="\n")
manifest = []
for file in sorted(ROOT.rglob("*")):
    if file.is_file():
        manifest.append(f"{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.relative_to(ROOT).as_posix()}\n")
(ROOT / "checksums.sha256").write_text("".join(manifest), encoding="ascii", newline="\n")
with tarfile.open(ARCHIVE, "w:gz") as archive:
    archive.add(ROOT, arcname=ROOT.name)
print(json.dumps(report, indent=2))
print("archive", ARCHIVE, ARCHIVE.stat().st_size)
print("sha256", hashlib.sha256(ARCHIVE.read_bytes()).hexdigest())
