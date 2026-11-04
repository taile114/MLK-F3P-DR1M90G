"""Freeze the single-primary native-block model for an offline F3P test package."""
from __future__ import annotations

import argparse
import copy
import csv
import json
from pathlib import Path
import shutil
import sys

import numpy as np
import torch
from torch.nn import functional as F

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "model"))
from fault_mtl.dataset import FaultDataset
from fault_mtl.evaluate import load_checkpoint
from fault_mtl.model import decode
from fault_mtl.schema import INPUT_SHAPES, SINGLE_CLASS_ORDER, file_hash, write_json


def round_away(x):
    return torch.sign(x) * torch.floor(torch.abs(x) + 0.5)


def batches(dataset, size=16):
    for start in range(0, len(dataset), size):
        samples = [dataset[i] for i in range(start, min(start + size, len(dataset)))]
        yield start, {k: torch.stack([s[k] for s in samples]) for k in INPUT_SHAPES}


def metrics(output, dataset):
    decoded = decode(output)
    c = decoded["class_index"].numpy()
    g = decoded["selected_grade"].numpy()
    tc = np.array([r["class_target"] for r in dataset.rows])
    tg = np.array([r["primary_grade_target"] for r in dataset.rows])
    fault = tc != 0
    return {"samples": len(c), "class_correct": int((c == tc).sum()),
            "joint_correct": int(((c == tc) & (g == tg)).sum()),
            "fault_grade_correct": int(((c == tc) & (g == tg) & fault).sum()),
            "fault_samples": int(fault.sum())}


class QuantizedCnn(torch.nn.Module):
    def __init__(self, model, scales):
        super().__init__()
        self.scales = scales
        self.weights, self.biases, self.bound = [], [], []
        for i, block in enumerate(model.backbone):
            sx, sw, sy = scales[i]
            w = round_away(block[0].weight.double() / sw).clamp(-128, 127)
            b = round_away(block[0].bias.double() / (sx * sw))
            if b.abs().max() >= 2**31:
                raise ValueError("Quantized bias overflow")
            # Integer convolution in float64 is exact for these bounded accumulators.
            self.weights.append(w)
            self.biases.append(b)
            self.bound.append(int((w.abs().sum((1, 2, 3)) * 128 + b.abs()).max()))
        self.last_bytes = None

    def forward(self, x):
        x = round_away(x.double() / self.scales[0][0]).clamp(-128, 127)
        for (sx, sw, sy), w, b in zip(self.scales, self.weights, self.biases):
            acc = F.conv2d(x, w, b, padding=1)
            x = round_away(acc * (sx * sw / sy)).clamp(0, 127)
            x = F.max_pool2d(x, 2)
        self.last_bytes = (x + 128).to(torch.uint8)
        return (x * self.scales[-1][2]).float()


@torch.no_grad()
def prepare(args):
    torch.set_num_threads(4)
    if args.output.exists() and any(args.output.iterdir()):
        raise ValueError("Output must be new or empty")
    model, checkpoint = load_checkpoint(args.checkpoint)
    if model.classification_mode != "single_label" or not model.temperature_isolation:
        raise ValueError("Expected the temperature-isolated single-primary model")
    for filename, key in (("dataset_manifest.json", "dataset_manifest_sha256"),
                          ("preprocess.json", "preprocess_sha256"),
                          ("label_schema.json", "label_schema_sha256")):
        if file_hash(args.dataset / filename) != checkpoint[key]:
            raise ValueError(f"Checkpoint identity mismatch: {filename}")
    datasets = {s: FaultDataset(args.dataset, s) for s in ("train", "val", "test")}
    folded = model.fused_eval()
    ranges = [0.] * 5
    for _, batch in batches(datasets["train"]):
        x = batch["time_frequency"]
        ranges[0] = max(ranges[0], float(x.abs().max()))
        for i, block in enumerate(folded.backbone):
            x = block(x)
            ranges[i + 1] = max(ranges[i + 1], float(x.max()))
    activation_scales = [float(np.float32(max(v, 1e-8) / 127)) for v in ranges]
    scales = []
    for i, block in enumerate(folded.backbone):
        sw = float(np.float32(float(block[0].weight.abs().max()) / 127))
        scales.append((activation_scales[i], sw, activation_scales[i + 1]))
    quantized = copy.deepcopy(folded)
    quantized.backbone = QuantizedCnn(folded, scales)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "data").mkdir()
    (args.output / "src").mkdir()
    for name in ("fault_test.cpp", "build.sh", "run_test.sh", "verify_results.py", "verify_integer.py", "README.md"):
        destination = args.output / ("src/fault_test.cpp" if name.endswith(".cpp") else name)
        destination.write_text((HERE / name).read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
    for name in ("preprocess.json", "label_schema.json", "config.yaml", "dataset_manifest.json", "split_blocks.csv"):
        shutil.copyfile(args.dataset / name, args.output / name)
    values, entries = [], []
    for name, tensor in folded.state_dict().items():
        array = tensor.numpy().astype("<f4")
        entries.append(f"{name} {array.ndim} " + " ".join(map(str, array.shape)) + f" {array.size} {sum(v.size for v in values)}")
        values.append(array.ravel())
    np.concatenate(values).tofile(args.output / "weights.bin")
    (args.output / "weights.tsv").write_text("\n".join(entries) + "\n", encoding="ascii")
    folded.thresholds().numpy().astype("<f4").tofile(args.output / "thresholds.bin")
    (args.output / "qparams.tsv").write_text("".join(f"{i} {sx:.17g} {sw:.17g} {sy:.17g}\n" for i, (sx, sw, sy) in enumerate(scales)), encoding="ascii")
    report = {"schema": "mlk.fault.offline_preparation.v1", "calibration_split": "train",
              "calibration_windows": len(datasets["train"]), "calibration_method": "symmetric_maxabs_u8_zp128_train_only",
              "activation_ranges": ranges, "scales": scales, "accumulator_abs_bounds": quantized.backbone.bound,
              "final_training": False, "acceptance_verified": False, "physical_npu_verified": False,
              "validation_scope": datasets["test"].manifest.get("validation_scope"),
              "independent_validation_available": False,
              "checkpoint_sha256": file_hash(args.checkpoint), "splits": {}}
    for split in ("val", "test"):
        dataset = datasets[split]
        original, folded_outputs, quant_outputs, quant_bytes = [], [], [], []
        max_fold_error = 0.
        with (args.output / "data" / f"{split}_inputs.bin").open("wb") as inputs:
            for _, batch in batches(dataset):
                torch.cat([batch[k].flatten(1) for k in INPUT_SHAPES], 1).numpy().astype("<f4").tofile(inputs)
                a, b, q = model.forward_batch(batch), folded.forward_batch(batch), quantized.forward_batch(batch)
                max_fold_error = max(max_fold_error, *(float((a[k] - b[k]).abs().max()) for k in ("class_logits", "severity_scores")))
                original.append(a)
                folded_outputs.append(b)
                quant_outputs.append(q)
                quant_bytes.append(quantized.backbone.last_bytes.numpy())
        combine = lambda outputs: {k: torch.cat([x[k] for x in outputs]) for k in outputs[0]}
        a, b, q = map(combine, (original, folded_outputs, quant_outputs))
        if max_fold_error > 1e-4:
            raise ValueError("BN folding tolerance exceeded")
        for tag, output in (("reference", a), ("quantized", q)):
            torch.cat([output["class_logits"], output["severity_scores"]], 1).numpy().astype("<f4").tofile(args.output / "data" / f"{split}_{tag}.bin")
        np.concatenate(quant_bytes).tofile(args.output / "data" / f"{split}_cnn_u8.bin")
        with (args.output / "data" / f"{split}_index.csv").open("w", encoding="utf-8", newline="") as f:
            fields = ["sample", "window_id", "source_sha256", "feature_sha256", "window_end_us", "class_target", "grade_target", "reference_class", "reference_grade", "quantized_class", "quantized_grade"]
            writer = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
            writer.writeheader()
            da, dq = decode(a), decode(q)
            for i, row in enumerate(dataset.rows):
                writer.writerow(dict(zip(fields, (i, row["window_id"], row["source_sha256"], row["feature_sha256"], row["window_end_us"], row["class_target"], row["primary_grade_target"], int(da["class_index"][i]), int(da["selected_grade"][i]), int(dq["class_index"][i]), int(dq["selected_grade"][i])))))
        changes = {k: int((decode(a)[k] != decode(q)[k]).sum()) for k in ("class_index", "selected_grade")}
        report["splits"][split] = {"fp32": metrics(a, dataset), "quantized": metrics(q, dataset),
            "bn_fold_max_abs": max_fold_error, "changes_vs_fp32": changes,
            "max_logit_abs_error": float((a["class_logits"] - q["class_logits"]).abs().max())}
        print(split, json.dumps(report["splits"][split]), flush=True)
    # Runtime board package only carries test vectors; validation stays in preparation evidence.
    for p in (args.output / "data").glob("val_*"):
        p.unlink()
    manifest = {"schema": "mlk.fault.offline_test.v1", "package_id": args.output.name,
                "architecture": checkpoint["architecture"], "task_definition": checkpoint["task_definition"],
                "classification_mode": "single_label", "temperature_isolation": True,
                "preprocessing_version": checkpoint["preprocessing_version"],
                "input_contract": "base6_single_native4s_flat_v1", "input_count": 27683,
                "input_shapes": {k: list(v) for k, v in INPUT_SHAPES.items()},
                "class_order": list(SINGLE_CLASS_ORDER), "test_samples": len(datasets["test"]),
                "input_stage": "frozen_normalized_features", "raw_preprocessing_on_board": False,
                "npu_plan": "four_strict_Alnpu_conv_cpu_relu_pool",
                "default_mode": "hybrid_split",
                "disabled_plan": "fused_Conv_Relu_MaxPool_u8_zp128_invalid_relu",
                "cpu_fallback_allowed": False, "hardware_execution_verified": False,
                "final_training": False, "acceptance_verified": False,
                "checkpoint_sha256": file_hash(args.checkpoint), "label_policy": checkpoint.get("label_policy", {})}
    write_json(args.output / "model.json", manifest)
    write_json(args.output / "preparation.json", report)
    print("prepared", args.output, flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--checkpoint", type=Path, default=HERE.parent / "model/runs/active_selection_single_v1_seed42/best.pt")
    p.add_argument("--dataset", type=Path, default=HERE.parent / "model/cache/active_selection_single_v1")
    p.add_argument("--output", type=Path, required=True)
    prepare(p.parse_args())
