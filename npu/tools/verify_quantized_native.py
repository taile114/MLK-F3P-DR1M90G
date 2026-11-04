"""Recompute diagnostic metrics and NPU execution evidence from saved replay files."""
from __future__ import annotations

import csv
import json
from pathlib import Path

import numpy as np
from verify_board_linear import execution_events, interrupts, require

ROOT = Path(__file__).resolve().parents[1]


def verify(recovered):
    with np.load(ROOT / "results/calibration/rul-uint8-v1/head-spline-validation.npz") as data:
        reference = data["baseline"].copy()
        target = data["labels"].copy()
        simulation = data["prediction"].copy()
    reports = {}
    for parent in [recovered, recovered / "val184"]:
        for directory in sorted(parent.glob("results_*")):
            mode = directory.name.split("_")[1]
            if "cpu_quantized" in directory.name:
                mode = "cpu_quantized"
            requested = 184 if parent.name == "val184" else 1
            exit_code = int((directory / "exit_code.txt").read_text())
            if exit_code != 0:
                log = (directory / "probe.log").read_text()
                require(mode == "hybrid" and requested == 184 and exit_code == 2
                        and "result=failed stage=inference error=cma alloc failed" in log,
                        "Unexpected native failure")
            with (directory / "predictions.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            count = len(rows)
            require(count == requested if exit_code == 0 else 0 < count < requested, "Native replay length differs")
            predictions = np.array([float(row["prediction"]) for row in rows], np.float32)
            refs = np.array([float(row["reference"]) for row in rows], np.float32)
            labels = np.array([float(row["target"]) for row in rows], np.float32)
            indices = np.arange(count)
            require(np.array_equal(refs, reference[indices]) and np.array_equal(labels, target[indices]), "Replay reference/label differs")
            require([int(row["sample_index"]) for row in rows] == list(range(count)) and np.isfinite(predictions).all(), "Replay indices/finite values differ")
            diff = predictions.astype(np.float64) - labels
            delta = predictions.astype(np.float64) - refs
            simulator_delta = predictions.astype(np.float64) - simulation[indices]
            if mode == "baseline":
                require(np.all(np.abs(delta) <= 1e-4 + 1e-4 * np.abs(refs)), "FP32 reconstruction tolerance exceeded")
            npu_layers, npu_events = 0, 0
            for index in range(6):
                dot = (directory / f"segment_{index}.dot").read_text()
                npu_layers += dot.count("LayerType : ALHardNPU")
                profile = json.loads((directory / f"segment_{index}_profile.json").read_text())
                events = [event for event in execution_events(profile) if "AlnpuALHardNPU" in event[0]]
                npu_events += sum(len(event[1]) for event in events)
            require(npu_layers == npu_events == (2 if mode == "hybrid" else 0), "Unexpected NPU graph/profile scope")
            before, after = (interrupts(directory / ("interrupts_" + tag + ".txt")) for tag in ["before", "after"])
            irq = {name: after[name] - before[name] for name in before}
            if mode == "hybrid":
                # Hardware tiling can generate multiple IRQs per graph layer.
                per_window = reports["first/hybrid"]["interrupt_delta"]["HARD_NPU_IRQ"] if requested > 1 else irq["HARD_NPU_IRQ"]
                require(per_window > 0 and per_window * count <= irq["HARD_NPU_IRQ"]
                        <= per_window * (count + (1 if exit_code else 0)), "Native IRQ scope differs")
            else:
                require(irq["HARD_NPU_IRQ"] == 0, "Unexpected CPU-run NPU IRQ")
            require(irq["SOFT_NPU_IRQ"] == 0, "Unexpected soft NPU IRQ")
            times = np.array([float(row["forward_us"]) for row in rows])
            result = {"samples": count, "requested_samples": requested, "exit_code": exit_code,
                      "failure": "CMA allocation failed during repeated inference" if exit_code else None,
                      "mode": mode, "rmse": float(np.sqrt(np.mean(diff ** 2))),
                      "mae": float(np.mean(np.abs(diff))), "fp32_reference_rmse": float(np.sqrt(np.mean(delta ** 2))),
                      "fp32_reference_max_abs": float(np.max(np.abs(delta))),
                      "quantized_pc_reference_max_abs": float(np.max(np.abs(simulator_delta))),
                      "mean_forward_us": float(times.mean()), "NPU_graph_layers": npu_layers,
                      "NPU_profiled_first_window_events": npu_events, "interrupt_delta": irq,
                      "application_accuracy_acceptance": "TBD"}
            if count > 1:
                result["after_first_window_diagnostic_us"] = {
                    "mean": float(times[1:].mean()), "p50": float(np.median(times[1:])),
                    "p95": float(np.percentile(times[1:], 95))}
            reports[("validation184/" if requested == 184 else "first/") + mode] = result
    require(set(reports) == {group + "/" + mode for group in ["first", "validation184"]
                            for mode in ["baseline", "cpu_quantized", "hybrid"]}, "Missing native comparisons")
    return {"status": "blocked_by_cma_allocation_failure", "runs": reports,
            "hybrid_first_window_verified": True, "full_model_hybrid_validation_replay_verified": False,
            "NPU_branches": ["output_layer.0.spline_weight", "output_layer.2.spline_weight"],
            "test_dataset_opened": False, "training_executed": False,
            "production_accuracy_accepted": False, "formal_warmed_performance_benchmark": False}
