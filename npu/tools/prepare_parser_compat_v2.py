"""Build shape/axis compatibility variants and independent ONNX operator controls."""
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
parser.add_argument("--output", type=Path, default=NPU_ROOT / "work/f3p-parser-compat-v2")
ROOT = parser.parse_args().output.resolve()
SOURCE = MODEL_ROOT / "outputs/motor_stable23_v1_onnx/rul_fp32.onnx"
ARCHIVE = ROOT.with_suffix(".tar.gz")
if ROOT.exists() or ARCHIVE.exists():
    raise SystemExit("Refusing to overwrite an existing package")
(ROOT / "models").mkdir(parents=True)
original_bytes = SOURCE.read_bytes()
original = onnx.load(SOURCE)
shaped = shape_inference.infer_shapes(original, strict_mode=True, data_prop=True)


def infos(model):
    return {x.name: x for x in [*model.graph.input, *model.graph.value_info, *model.graph.output]}


def dims(info):
    result = [d.dim_value for d in info.type.tensor_type.shape.dim]
    if any(d <= 0 for d in result):
        raise ValueError("Expected statically known positive tensor dimensions")
    return result


def positive_axes(model, include_attributes):
    result = copy.deepcopy(model)
    shapes = infos(result)
    constants = {t.name: numpy_helper.to_array(t) for t in result.graph.initializer}
    changes = []
    for i, node in enumerate(result.graph.node):
        if node.op_type == "Unsqueeze":
            axes = constants[node.input[1]].reshape(-1)
            rank = len(dims(shapes[node.input[0]])) + axes.size
            normalized = [int(axis + rank if axis < 0 else axis) for axis in axes]
            if len(set(normalized)) != len(normalized) or any(x < 0 or x >= rank for x in normalized):
                raise ValueError("Invalid Unsqueeze axes")
            if normalized != axes.tolist():
                # A shared initializer may also be used by Reshape or another rank.
                # Create a dedicated constant instead of changing the shared value.
                name = f"f3p_positive_unsqueeze_axes_{i}"
                result.graph.initializer.append(helper.make_tensor(name, TensorProto.INT64, [len(normalized)], normalized))
                node.input[1] = name
                changes.append({"node": node.name, "field": "Unsqueeze.axes", "before": axes.tolist(), "after": normalized})
        if include_attributes and node.op_type in {"Softmax", "Concat", "ReduceMean", "ReduceL2", "Gather"}:
            rank = len(dims(shapes[node.input[0]]))
            for attribute in node.attribute:
                if attribute.name == "axis" and attribute.i < 0:
                    before = attribute.i
                    attribute.i += rank
                    if not 0 <= attribute.i < rank:
                        raise ValueError("Invalid axis")
                    changes.append({"node": node.name, "field": "axis", "before": before, "after": attribute.i})
                if attribute.name == "axes" and any(x < 0 for x in attribute.ints):
                    before = list(attribute.ints)
                    normalized = [x + rank if x < 0 else x for x in before]
                    if any(x < 0 or x >= rank for x in normalized):
                        raise ValueError("Invalid axes")
                    del attribute.ints[:]
                    attribute.ints.extend(normalized)
                    changes.append({"node": node.name, "field": "axes", "before": before, "after": normalized})
    return result, changes


saved = []


def save(name, model, description):
    onnx.checker.check_model(model, full_check=True)
    onnx.save(model, ROOT / "models" / name)
    saved.append({"name": name, "description": description})


unsqueeze_fixed, fixes = positive_axes(shaped, False)
all_axes_fixed, all_fixes = positive_axes(shaped, True)
save("90_full_unsqueeze_positive.onnx", unsqueeze_fixed, "Full model: inferred tensor metadata and positive Unsqueeze axes")
save("91_full_all_axes_positive.onnx", all_axes_fixed, "Full model: inferred tensor metadata and all supported negative axis attributes normalized")


def prefix(model, count):
    lookup = infos(model)
    nodes = list(model.graph.node[:count])
    used = {x for n in nodes for x in n.input}
    output = nodes[-1].output[0]
    intermediates = [lookup[x] for n in nodes for x in n.output if x != output]
    graph = helper.make_graph(nodes, f"prefix_{count}", list(model.graph.input), [lookup[output]],
                              initializer=[t for t in model.graph.initializer if t.name in used],
                              value_info=intermediates)
    return helper.make_model(graph, opset_imports=list(model.opset_import), ir_version=model.ir_version)


save("00_reshape_with_shapes.onnx", prefix(shaped, 2), "Original first two nodes with intermediate tensor metadata")
save("01_unsqueeze_negative_with_shapes.onnx", prefix(shaped, 3), "Original negative-axis Unsqueeze with intermediate tensor metadata")
save("02_unsqueeze_positive_with_shapes.onnx", prefix(unsqueeze_fixed, 3), "Same prefix with Unsqueeze axis -1 replaced by 2")
save("03_less_prefix_with_shapes.onnx", prefix(unsqueeze_fixed, 4), "Original Less node following the corrected prefix")


def value(name, shape, dtype=TensorProto.FLOAT):
    return helper.make_tensor_value_info(name, dtype, shape)


def control(name, operator, inputs, outputs, attributes=None, constants=None):
    model = helper.make_model(helper.make_graph(
        [helper.make_node(operator, [x.name for x in inputs] + [t.name for t in constants or []],
                          [x.name for x in outputs], name="control_" + operator, **(attributes or {}))],
        "control_" + operator, inputs, outputs, initializer=constants or []),
        opset_imports=[helper.make_opsetid("", 13)], ir_version=7)
    save(name, shape_inference.infer_shapes(model, strict_mode=True), "Independent " + operator + " control")


control("10_less.onnx", "Less", [value("x", [2, 3]), value("y", [2, 3])], [value("out", [2, 3], TensorProto.BOOL)])
control("11_greater_equal.onnx", "GreaterOrEqual", [value("x", [2, 3]), value("y", [2, 3])], [value("out", [2, 3], TensorProto.BOOL)])
control("12_and.onnx", "And", [value("x", [2, 3], TensorProto.BOOL), value("y", [2, 3], TensorProto.BOOL)], [value("out", [2, 3], TensorProto.BOOL)])
control("13_cast_bool_float.onnx", "Cast", [value("x", [2, 3], TensorProto.BOOL)], [value("out", [2, 3])], {"to": TensorProto.FLOAT})
control("14_erf.onnx", "Erf", [value("x", [2, 3])], [value("out", [2, 3])])
control("15_reduce_l2.onnx", "ReduceL2", [value("x", [2, 3])], [value("out", [2, 1])], {"axes": [1], "keepdims": 1})
control("16_einsum_const.onnx", "Einsum", [value("x", [2, 3, 4])], [value("out", [2, 5])],
        {"equation": "bik,oik->bo"}, [numpy_helper.from_array(np.linspace(-1, 1, 60, dtype=np.float32).reshape(5, 3, 4), "weights")])
control("17_matmul_dynamic_2d.onnx", "MatMul", [value("x", [11, 12]), value("y", [12, 21])], [value("out", [11, 21])])
control("18_matmul_dynamic_3d.onnx", "MatMul", [value("x", [1, 11, 21]), value("y", [1, 21, 11])], [value("out", [1, 11, 11])])
weight = numpy_helper.from_array(np.linspace(-1, 1, 252, dtype=np.float32).reshape(12, 21), "weights")
control("19_matmul_const_2d.onnx", "MatMul", [value("x", [11, 12])], [value("out", [11, 21])], constants=[weight])
control("20_gemm_const_2d.onnx", "Gemm", [value("x", [11, 12])], [value("out", [11, 21])], constants=[weight])

options = ort.SessionOptions()
options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
options.intra_op_num_threads = 1
options.inter_op_num_threads = 1
data_dir = MODEL_ROOT / "processed/run_to_failure_5motors_v1/board_replay"
windows = np.fromfile(data_dir / "test_input_f32.bin", dtype="<f4").reshape(-1, 23, 110)
with np.load(MODEL_ROOT / "outputs/motor_stable23_v1_onnx/predictions.npz") as predictions:
    reference = predictions["pytorch_cpu"].reshape(-1)
for entry in saved:
    session = ort.InferenceSession(str(ROOT / "models" / entry["name"]), sess_options=options, providers=["CPUExecutionProvider"])
    feeds = {}
    for item in session.get_inputs():
        if item.name == "features":
            feeds[item.name] = windows[:1]
        elif item.type == "tensor(bool)":
            feeds[item.name] = (np.arange(np.prod(item.shape)).reshape(item.shape) % 2 == 0)
        else:
            feeds[item.name] = np.linspace(-1, 1, np.prod(item.shape), dtype=np.float32).reshape(item.shape)
    session.run(None, feeds)
    entry.update(checker="passed", ort="passed")
    if entry["name"].startswith(("90_", "91_")):
        predictions = np.asarray([session.run(None, {"features": x[None]})[0].item() for x in windows])
        errors = np.abs(predictions - reference)
        if not np.all(errors <= 1e-4 + 1e-4 * np.abs(reference)):
            raise RuntimeError("Full-model reference mismatch")
        np.savez(ROOT / entry["name"].replace(".onnx", "_predictions.npz"), prediction=predictions, reference=reference)
        entry.update(samples=len(windows), max_reference_error=float(errors.max()))
        converted = onnx.load(ROOT / "models" / entry["name"])
        # Every original parameter/constant must retain its name, type, shape and value.
        new_constants = {t.name: t for t in converted.graph.initializer}
        for tensor in original.graph.initializer:
            assert tensor.SerializeToString() == new_constants[tensor.name].SerializeToString(), tensor.name
        entry["original_initializers_unchanged"] = len(original.graph.initializer)

report = {"source_sha256": hashlib.sha256(original_bytes).hexdigest(), "unsqueeze_fixes": fixes,
          "all_axis_fixes": all_fixes, "models": saved, "vendor_board_parse_verified": False,
          "scope": "shape metadata and equivalent axis encoding only; no quantization or NPU verification"}
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
echo "results=$results"
for model in "$here"/models/*.onnx; do
    name=$(basename "$model" .onnx)
    status=0
    "$probe/bin/rul_probe" --model "$model" --parse-only > "$results/$name.log" 2>&1 || status=$?
    printf '%s exit_code=%s\\n' "$name" "$status"
    if [ "$status" -ne 0 ]; then tail -n 4 "$results/$name.log"; fi
    printf '%s %s\\n' "$name" "$status" >> "$results/summary.txt"
done
echo "--- summary ---"
cat "$results/summary.txt"
echo "results=$results"
'''
(ROOT / "run_diagnostics.sh").write_text(script, encoding="utf-8", newline="\n")
(ROOT / "README.txt").write_text(
    "F3P parser compatibility controls, version 2.\n"
    "Requires the original /root/f3p-rul-probe executable and libraries.\n"
    "Run sha256sum -c checksums.sha256, then sh run_diagnostics.sh on board.\n"
    "00-03 isolate tensor metadata and negative/positive Unsqueeze axes.\n"
    "10-20 independently check comparisons, boolean operations, Erf, ReduceL2, Einsum and matrix multiplication.\n"
    "90-91 are complete model variants; see verification.json for equivalent axis changes.\n"
    "All 302 original initializers are unchanged. Both complete variants passed all 165 reference windows on PC.\n"
    "ONNX standard allows negative Unsqueeze axes: https://onnx.ai/onnx/operators/onnx__Unsqueeze.html\n"
    "No board inference or NPU acceleration has been verified by this package.\n", encoding="utf-8", newline="\n")
manifest = []
for file in sorted(ROOT.rglob("*")):
    if file.is_file():
        manifest.append(f"{hashlib.sha256(file.read_bytes()).hexdigest()}  {file.relative_to(ROOT).as_posix()}\n")
(ROOT / "checksums.sha256").write_text("".join(manifest), encoding="ascii", newline="\n")
with tarfile.open(ARCHIVE, "w:gz", format=tarfile.GNU_FORMAT) as archive:
    # Older board clocks do not need future-mtime warnings for diagnostic data.
    def stable_time(member):
        member.mtime = 0
        return member
    archive.add(ROOT, arcname=ROOT.name, filter=stable_time)
assert SOURCE.read_bytes() == original_bytes
print(json.dumps({"models": len(saved), "unsqueeze_fixes": len(fixes), "all_axis_fixes": len(all_fixes),
                  "full_model_validation": [x for x in saved if x["name"].startswith(("90_", "91_"))],
                  "archive": str(ARCHIVE), "bytes": ARCHIVE.stat().st_size,
                  "sha256": hashlib.sha256(ARCHIVE.read_bytes()).hexdigest()}, indent=2))
