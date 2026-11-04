"""Independently recompute captured hard-NPU convolutions using int64 arithmetic."""
import argparse
import hashlib
import json
from pathlib import Path
import re

import numpy as np


def round_away(value):
    return np.sign(value) * np.floor(np.abs(value) + 0.5)


def audit(bundle, result):
    values = np.fromfile(bundle / "weights.bin", dtype="<f4")
    weights = {}
    for line in (bundle / "weights.tsv").read_text().splitlines():
        fields = line.split()
        rank = int(fields[1])
        shape = tuple(map(int, fields[2:2 + rank]))
        count, offset = map(int, fields[2 + rank:])
        weights[fields[0]] = values[offset:offset + count].reshape(shape).astype(np.float64)
    scales = [tuple(map(float, line.split()[1:])) for line in (bundle / "qparams.tsv").read_text().splitlines()]
    channels = (8, 16, 32, 64, 64)
    checks = []
    for path in sorted(result.glob("audit_cnn_*_sample*_input.bin")):
        match = re.fullmatch(r"audit_cnn_(\d)_(\d)_sample(\d+)_input.bin", path.name)
        if not match or int(match[2]) != int(match[1]) + 1:
            raise ValueError("Expected a single-convolution capture")
        layer, sample = int(match[1]), int(match[3])
        if not 0 <= layer < 4 or not 0 <= sample < 360:
            raise ValueError("Audit index outside the frozen model")
        h, width = 64 >> layer, 48 >> layer
        actual_input = np.fromfile(path, dtype=np.uint8).reshape(channels[layer], h, width)
        output_path = path.with_name(path.name.replace("_input.bin", "_output.bin"))
        actual_output = np.fromfile(output_path, dtype=np.uint8).reshape(channels[layer + 1], h, width)
        sx, sw, sy = scales[layer]
        qw = np.clip(round_away(weights[f"backbone.{layer}.0.weight"] / sw), -128, 127).astype(np.int64)
        qb = round_away(weights[f"backbone.{layer}.0.bias"] / (sx * sw)).astype(np.int64)
        centered = np.pad(actual_input.astype(np.int64) - 128, ((0, 0), (1, 1), (1, 1)))
        windows = np.lib.stride_tricks.sliding_window_view(centered, (3, 3), axis=(1, 2))
        columns = windows.transpose(1, 2, 0, 3, 4).reshape(h * width, -1)
        accumulators = columns @ qw.reshape(channels[layer + 1], -1).T + qb
        expected = np.clip(round_away(accumulators.astype(np.float64) * (sx * sw / sy)) + 128, 0, 255)
        expected = expected.T.reshape(channels[layer + 1], h, width).astype(np.int16)
        error = actual_output.astype(np.int16) - expected
        maximum = int(np.abs(error).max())
        checks.append({"sample": sample, "layer": layer, "outputs": int(error.size),
            "max_error_lsb": maximum, "signed_mean_error_lsb": float(error.mean()),
            "different_outputs": int(np.count_nonzero(error)),
            "input_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "output_sha256": hashlib.sha256(output_path.read_bytes()).hexdigest()})
    if not checks:
        raise ValueError("Missing actual hard-NPU IO captures")
    summary = json.loads((result / "summary.json").read_text())
    required_samples = {i for i in (0, 84, 150, 200, 246, 300, 359) if i < summary["samples"]}
    if {(x["sample"], x["layer"]) for x in checks} != {(i, layer) for i in required_samples for layer in range(4)}:
        raise ValueError("Missing selected-window convolution captures")
    report = {"schema": "mlk.fault.hard_npu_integer_audit.v1", "convolutions": len(checks),
        "outputs_checked": sum(x["outputs"] for x in checks),
        "maximum_error_lsb": max(x["max_error_lsb"] for x in checks),
        "actual_input_used": True, "integer_accumulator": "int64", "scales_fitted_again": False,
        "weights_sha256": hashlib.sha256((bundle / "weights.bin").read_bytes()).hexdigest(),
        "qparams_sha256": hashlib.sha256((bundle / "qparams.tsv").read_bytes()).hexdigest(), "checks": checks}
    (result / "integer_audit.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["maximum_error_lsb"] > 1:
        raise ValueError("Hard-NPU convolution exceeded the 1-LSB integer tolerance")
    print(json.dumps({k: v for k, v in report.items() if k != "checks"}, indent=2))
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    args = parser.parse_args()
    audit(args.bundle, args.results)
