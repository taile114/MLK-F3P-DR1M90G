"""Export real trained branch weights and validation fixtures, without training."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys
import tarfile

import numpy as np
import torch
import torch.nn.functional as F

from calibrate_rul_uint8 import ROOT, CHECKPOINT_SHA256, digest, quantize, rounded, write_json
sys.path.insert(0, str(ROOT / "training"))
from fkp_dvgt.model import DVGTformer_model
from fkp_dvgt.data import load_dataset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration", type=Path, default=ROOT / "results/calibration/rul-uint8-v1/calibration.json")
    parser.add_argument("--output", type=Path, default=ROOT / "work/f3p-real-weight-v1")
    args = parser.parse_args()
    calibration = json.loads(args.calibration.read_text())
    checkpoint = ROOT / "training/outputs/motor_stable23_v1_gpu/best.pt"
    if digest(checkpoint) != calibration["checkpoint_sha256"] or digest(checkpoint) != CHECKPOINT_SHA256:
        raise ValueError("Checkpoint mismatch")
    output = args.output.resolve()
    source_archive = output.with_name(output.name + ".source.tar.gz")
    if output.exists() or source_archive.exists():
        raise FileExistsError(output)
    torch.set_num_threads(1)
    torch.manual_seed(44)
    state = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model = DVGTformer_model(**state["config"]["model"])
    model.load_state_dict(state["model_state_dict"], strict=True)
    model.eval().requires_grad_(False)
    val_path = ROOT / "training/processed/run_to_failure_5motors_v1/val.npz"
    if digest(val_path) != calibration["validation_sha256"]:
        raise ValueError("Validation source mismatch")
    x, _ = load_dataset(val_path).tensors
    indices = [0, len(x) // 2, len(x) - 1]
    selected = ["tvgtformer_blocks.0.linears_Q_temp.0.base_weight",
                "tvgtformer_blocks.0.linears_Q_temp.0.spline_weight",
                "linear_x.base_weight", "linear_x.spline_weight",
                "output_layer.0.spline_weight", "output_layer.2.spline_weight"]
    captured = {}
    handles = []
    for module_name in sorted({calibration["layers"][key]["module"] for key in selected}):
        def capture(module, inputs, name=module_name):
            flat = inputs[0].reshape(-1, module.in_features)
            captured[name + ".base_weight"] = (module.base_activation(flat).clone(), module.base_weight.clone())
            captured[name + ".spline_weight"] = (module.b_splines(flat).flatten(1).clone(), module.spline_weight.flatten(1).clone())
        handles.append(model.get_submodule(module_name).register_forward_pre_hook(capture))
    with torch.inference_mode():
        model(x[indices], graph_probability=0.0)
    for handle in handles:
        handle.remove()
    (output / "cases").mkdir(parents=True)
    (output / "src").mkdir()
    (output / "bin").mkdir()
    index = {}
    for number, key in enumerate(selected):
        entry = calibration["layers"][key]
        values, weights = captured[key]
        rows = len(values) // 3
        inputs, outputs = entry["input_features"], entry["output_features"]
        ic, oc = (inputs + 7) // 8 * 8, (outputs + 7) // 8 * 8
        sx, sw, sy = (entry[k] for k in ["input_scale", "weight_scale", "output_scale"])
        qx = quantize(values, sx).reshape(3, rows, inputs).numpy().astype(np.uint8)
        qw = quantize(weights, sw).numpy().astype(np.uint8)
        accumulator = (qx.astype(np.int64) - 128) @ (qw.astype(np.int64) - 128).T
        multiplier = sx * sw / sy
        scaled = torch.from_numpy(accumulator.astype(np.float64) * multiplier)
        qy = (rounded(scaled) + 128).clamp(0, 255).numpy().astype(np.uint8)
        padded_x, padded_w, padded_y = np.full((3, rows, ic), 128, np.uint8), np.full((oc, ic), 128, np.uint8), np.full((3, rows, oc), 128, np.uint8)
        padded_x[:, :, :inputs], padded_w[:outputs, :inputs], padded_y[:, :, :outputs] = qx, qw, qy
        name = f"w{number:02}"
        directory = output / "cases" / name
        directory.mkdir()
        padded_x.tofile(directory / "inputs_u8.bin")
        padded_w.tofile(directory / "weights_u8.bin")
        padded_y.tofile(directory / "expected_u8.bin")
        F.linear(values, weights).numpy().astype("<f4").tofile(directory / "reference_f32.bin")
        (directory / "case.txt").write_text(f"{rows} {inputs} {outputs} 3 {sx:.17g} {sw:.17g} {sy:.17g}\n", encoding="ascii", newline="\n")
        index[name] = {"layer": key, "rows": rows, "physical_input_features": ic, "physical_output_features": oc, **entry}
    for source, target in [("npu_real_weight.cpp", "src/npu_real_weight.cpp"),
                           ("npu_linear_padded.cpp", "src/npu_linear_padded.cpp"),
                           ("build_npu_real_weight.sh", "build.sh"), ("run_npu_real_weight.sh", "run.sh")]:
        shutil.copyfile(ROOT / "probe" / source, output / target)
    with tarfile.open(ROOT / "artifacts/f3p-npu-fc-pad-arm64-v1.tar.gz") as archive:
        (output / "bin/timeout").write_bytes(archive.extractfile("f3p-npu-fc-pad-v1/bin/timeout").read())
    for group, names in [("first", ["w00"]), ("all", list(index))]:
        (output / (group + "_cases.txt")).write_text("\n".join(names) + "\n", encoding="ascii", newline="\n")
    write_json(output / "preparation.json", {"checkpoint_sha256": CHECKPOINT_SHA256,
        "calibration_sha256": digest(args.calibration), "validation_indices": indices,
        "train_only_scales": True, "test_dataset_opened": False, "training_executed": False, "cases": index})
    with tarfile.open(source_archive, "w:gz") as archive:
        for path in sorted(output.rglob("*")):
            archive.add(path, arcname=(Path(output.name) / path.relative_to(output)).as_posix(), recursive=False)
    print(json.dumps({"source": str(source_archive), "bytes": source_archive.stat().st_size, "cases": len(index)}))


if __name__ == "__main__":
    main()
