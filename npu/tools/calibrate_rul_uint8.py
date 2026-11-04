"""Frozen-weight UINT8 calibration and validation diagnostics; never train."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import types

import numpy as np
import torch
import torch.nn.functional as F

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "training"))
from fkp_dvgt.data import load_dataset
from fkp_dvgt.model import DVGTformer_model, KANLayer

CHECKPOINT_SHA256 = "bb7cb39da623e95574e57a559f1df979e5ee5ddea75d71d079f818da108d16f2"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")


def rounded(value):
    return torch.sign(value) * torch.floor(torch.abs(value) + 0.5)


def quantize(value, scale):
    return (rounded(value / scale) + 128).clamp(0, 255)


def scale_for(maximum, power_two=False):
    value = max(float(maximum) / 127, 2.0 ** -30)
    return float(np.float32(2.0 ** math.ceil(math.log2(value)) if power_two else value))


def metrics(prediction, labels, baseline=None):
    error = prediction.double() - labels.double()
    result = {"rmse": error.square().mean().sqrt().item(), "mae": error.abs().mean().item()}
    if baseline is not None:
        delta = prediction.double() - baseline.double()
        result.update(reference_rmse=delta.square().mean().sqrt().item(),
                      reference_max_abs=delta.abs().max().item(),
                      reference_mean_abs=delta.abs().mean().item())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "work/rul-uint8-calibration-v1")
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--isolated", type=int, default=124, help="Maximum isolated branch diagnostics, not training")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    checkpoint = ROOT / "training/outputs/motor_stable23_v1_gpu/best.pt"
    if digest(checkpoint) != CHECKPOINT_SHA256:
        raise ValueError("Checkpoint differs from the verified deployment baseline")
    torch.set_num_threads(1)
    torch.manual_seed(44)
    state = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model = DVGTformer_model(**state["config"]["model"])
    model.load_state_dict(state["model_state_dict"], strict=True)
    model.eval().requires_grad_(False)
    initial = {name: value.clone() for name, value in model.state_dict().items()}
    data_root = ROOT / "training/processed/run_to_failure_5motors_v1"
    train_path, val_path = data_root / "train.npz", data_root / "val.npz"
    train_x, _ = load_dataset(train_path).tensors
    val_x, val_y = load_dataset(val_path).tensors
    if (len(train_x), len(val_x)) != (543, 184):
        raise ValueError("Unexpected calibration/validation split")
    records, stats, fixtures = {}, {}, {}
    active, selected, mode = set(), set(), "calibrate"
    weights_cache = {}

    def branch(module_name, kind, x, weight):
        key = module_name + "." + kind
        ref = F.linear(x, weight)
        if mode == "calibrate":
            active.add(module_name)
            entry = records.setdefault(key, {"module": module_name, "branch": kind,
                                            "input_features": x.shape[-1], "output_features": weight.shape[0],
                                            "input_absmax": 0.0, "output_absmax": 0.0,
                                            "weight_absmax": float(weight.abs().max()), "calibration_rows": 0})
            entry["input_absmax"] = max(entry["input_absmax"], x.abs().max().item())
            entry["output_absmax"] = max(entry["output_absmax"], ref.abs().max().item())
            entry["calibration_rows"] += len(x)
            return ref
        entry = records[key]
        if key not in selected and mode != "local":
            return ref
        sx, sw, sy = (entry[name] for name in ["input_scale", "weight_scale", "output_scale"])
        qx = quantize(x, sx)
        qw = weights_cache[key]
        # Products and sums of small integer-valued doubles are exact at these dimensions.
        accumulator = (qx.double() - 128) @ (qw.double() - 128).T
        qy = (rounded(accumulator * (float(sx) * float(sw) / float(sy))) + 128).clamp(0, 255)
        approximate = ((qy - 128) * sy).to(torch.float32)
        if mode == "local":
            delta = approximate.double() - ref.double()
            entry_stats = stats.setdefault(key, {"count": 0, "squared_error": 0.0, "reference_squared": 0.0,
                                                "max_abs_error": 0.0, "input_clipped": 0, "input_count": 0,
                                                "output_clipped": 0, "output_count": 0})
            entry_stats["count"] += delta.numel()
            entry_stats["squared_error"] += delta.square().sum().item()
            entry_stats["reference_squared"] += ref.double().square().sum().item()
            entry_stats["max_abs_error"] = max(entry_stats["max_abs_error"], delta.abs().max().item())
            entry_stats["input_clipped"] += ((rounded(x / sx) < -128) | (rounded(x / sx) > 127)).sum().item()
            entry_stats["input_count"] += x.numel()
            pre_output = rounded(accumulator * (float(sx) * float(sw) / float(sy)))
            entry_stats["output_clipped"] += ((pre_output < -128) | (pre_output > 127)).sum().item()
            entry_stats["output_count"] += ref.numel()
            if key not in fixtures:
                rows = len(x) // args.batch_size if len(x) % args.batch_size == 0 else len(x)
                # Validation batch boundaries are irrelevant; preserve one window's native row count.
                fixtures[key] = (x[:rows].contiguous().clone(), weight.contiguous().clone())
            return ref
        return approximate

    for name, module in model.named_modules():
        if not isinstance(module, KANLayer):
            continue
        def forward(this, x, module_name=name):
            shape = x.shape
            flat = x.reshape(-1, this.in_features)
            base = branch(module_name, "base_weight", this.base_activation(flat), this.base_weight)
            spline = branch(module_name, "spline_weight", this.b_splines(flat).flatten(1), this.spline_weight.flatten(1))
            return (base + spline).reshape(*shape[:-1], this.out_features)
        module.forward = types.MethodType(forward, module)

    def predict(x):
        values = []
        with torch.inference_mode():
            for index in range(0, len(x), args.batch_size):
                values.append(model(x[index:index + args.batch_size], graph_probability=0.0).flatten())
        return torch.cat(values)

    # No optimizer, backward call or stochastic inference is used anywhere in this tool.
    predict(train_x)
    for key, entry in records.items():
        entry.update(input_scale=scale_for(entry["input_absmax"]), weight_scale=scale_for(entry["weight_absmax"]),
                     output_scale=scale_for(entry["output_absmax"]), zero_point=128)
        module = model.get_submodule(entry["module"])
        weight = getattr(module, entry["branch"]).flatten(1)
        weights_cache[key] = quantize(weight, entry["weight_scale"])
    if len(records) != 124:
        raise ValueError("Expected 124 active linear branches")
    write_json(output / "calibration.json", {
        "scope": "Per-tensor symmetric QAsymmU8 calibration from frozen FP32 training activations only",
        "checkpoint_sha256": CHECKPOINT_SHA256, "train_sha256": digest(train_path),
        "validation_sha256": digest(val_path), "calibration_samples": 543,
        "normalization": "Frozen processed arrays, train-only normalization already applied; no refit",
        "test_dataset_opened": False, "training_executed": False, "layers": records})
    print(json.dumps({"stage": "calibrated", "branches": len(records), "train_samples": 543}), flush=True)

    mode = "local"
    baseline = predict(val_x)
    original = DVGTformer_model(**state["config"]["model"])
    original.load_state_dict(state["model_state_dict"], strict=True)
    original.eval().requires_grad_(False)
    with torch.inference_mode():
        original_prediction = torch.cat([original(x, graph_probability=0.0).flatten()
            for x in val_x.split(args.batch_size)])
    reconstruction_error = (baseline - original_prediction).abs().max().item()
    if reconstruction_error > 1e-4:
        raise ValueError("FP32 linear/einsum reconstruction differs")
    baseline_metrics = metrics(baseline, val_y.flatten())
    local = {}
    for key, value in stats.items():
        local[key] = {**value, "rmse": math.sqrt(value["squared_error"] / value["count"]),
                      "relative_rms_error": math.sqrt(value["squared_error"] / max(value["reference_squared"], 1e-30))}
    write_json(output / "local-layer-errors.json", local)
    mode = "simulate"
    isolated = []
    # Each branch is quantized alone; all others remain FP32 for attribution.
    for index, key in enumerate(sorted(records)[:args.isolated]):
        selected = {key}
        prediction = predict(val_x)
        row = {"layer": key, **metrics(prediction, val_y.flatten(), baseline)}
        isolated.append(row)
        write_json(output / "isolated-validation.json", isolated)
        print(json.dumps({"stage": "isolated", "index": index + 1, "total": min(args.isolated, len(records)), **row}), flush=True)

    configurations = {"all": set(records),
                      "spline_only": {key for key in records if key.endswith(".spline_weight")},
                      "base_only": {key for key in records if key.endswith(".base_weight")},
                      "output_head": {key for key in records if key.startswith("output_layer.")},
                      "input_projection": {key for key in records if key.startswith("linear_x.")}}
    # Fixed error cutoffs are screening diagnostics, not application acceptance thresholds.
    for cutoff in [0.005, 0.02, 0.05]:
        configurations["isolated_reference_rmse_le_" + str(cutoff)] = {
            row["layer"] for row in isolated if row["reference_rmse"] <= cutoff}
    combined = {}
    for name, keys in configurations.items():
        selected = keys
        prediction = predict(val_x)
        combined[name] = {"layers": sorted(keys), "branches": len(keys),
                          **metrics(prediction, val_y.flatten(), baseline)}
        np.savez_compressed(output / (name + "-validation.npz"), prediction=prediction.numpy(),
                            baseline=baseline.numpy(), labels=val_y.numpy().flatten())
        print(json.dumps({"stage": "combined", "configuration": name,
                          **{k: v for k, v in combined[name].items() if k != "layers"}}), flush=True)
    fixture_root = output / "fixtures"
    fixture_root.mkdir()
    fixture_index = {}
    for index, (key, (x, weight)) in enumerate(sorted(fixtures.items())):
        directory = fixture_root / f"layer_{index:03}"
        directory.mkdir()
        entry = records[key]
        x.numpy().astype("<f4").tofile(directory / "input_f32.bin")
        weight.numpy().astype("<f4").tofile(directory / "weight_f32.bin")
        F.linear(x, weight).numpy().astype("<f4").tofile(directory / "reference_f32.bin")
        fixture_index[key] = {"directory": directory.name, "rows": len(x), **entry}
    write_json(output / "fixtures.json", fixture_index)
    for name, value in model.state_dict().items():
        if not torch.equal(value, initial[name]):
            raise ValueError("Frozen state changed: " + name)
    if digest(checkpoint) != CHECKPOINT_SHA256:
        raise ValueError("Checkpoint file changed")
    report = {"status": "passed", "checkpoint_sha256": CHECKPOINT_SHA256,
              "baseline_validation": baseline_metrics, "fp32_reconstruction_max_abs": reconstruction_error,
              "calibration_samples": 543, "validation_samples": 184, "active_linear_branches": len(records),
              "isolated_branches_evaluated": len(isolated), "combined": combined,
              "rounding": "Half away from zero; int-valued float64 GEMM, then output requantization",
              "hardware_rounding_equivalence_verified": False, "hardware_real_weights_verified": False,
              "full_rul_npu_verified": False, "test_dataset_opened": False, "training_executed": False,
              "checkpoint_state_unchanged": True, "application_accuracy_threshold": "TBD"}
    write_json(output / "verification.json", report)
    print(json.dumps({"stage": "completed", "output": str(output)}), flush=True)


if __name__ == "__main__":
    main()
