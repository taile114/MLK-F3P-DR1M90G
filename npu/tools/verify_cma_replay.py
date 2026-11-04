"""Verify saved CMA diagnostics and validation replay; never execute a model."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re

import numpy as np

from verify_board_linear import execution_events, interrupts, require

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = "5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce"


def rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def memory_run(directory, count, hybrid, fixed):
    records = rows(directory / "memory.csv")
    for record in records:
        for key in record.keys() - {"event"}:
            record[key] = int(record[key])
    require(len(records) == 6 + 12 * count + 2, "Incomplete per-segment memory trace")
    after = [r for r in records if r["event"] == "after_enqueue" and r["stage"] == 5]
    require([r["sample"] for r in after] == list(range(count)), "Memory sample order differs")
    allocations = [r["allocate_calls"] for r in after]
    totals = [r["cma_bytes"] for r in after]
    if fixed:
        require(len(set(allocations)) == len(set(totals)) == 1, "Repeated replay allocates additional CMA")
        if hybrid:
            require(totals[0] == 1204224 and allocations[0] == 10, "Unexpected fixed working-memory scope")
            early = [r for r in records if r["sample"] == 0 and r["event"] == "after_enqueue" and r["stage"] < 4]
            require(all(r["allocate_calls"] == 0 for r in early), "CPU-only segment used custom CMA")
        else:
            require(totals[0] == allocations[0] == 0, "CPU control used custom CMA")
    else:
        require(hybrid and allocations[-1] == 50 * count and after[-1]["live_blocks"] == 12 * count + 3,
                "Shared-runtime allocation-growth diagnostic differs")
        require(after[-1]["cma_bytes"] - after[0]["cma_bytes"] == 6455296 * (count - 1),
                "Shared-runtime CMA growth differs")
    return {
        "per_segment_records": len(records),
        "first_window_cma_bytes": totals[0], "last_window_cma_bytes": totals[-1],
        "first_window_allocator_calls": allocations[0], "last_window_allocator_calls": allocations[-1],
        "additional_cma_bytes_after_first_window": totals[-1] - totals[0],
        "additional_allocator_calls_after_first_window": allocations[-1] - allocations[0],
        "last_window_live_allocator_bytes": after[-1]["live_bytes"],
        "last_window_live_allocator_blocks": after[-1]["live_blocks"],
        "after_unload_cma_bytes_before_session_cleanup": records[-1]["cma_bytes"],
        "after_unload_live_blocks_before_session_cleanup": records[-1]["live_blocks"],
        "minimum_cma_free_kb": min(r["cma_free_kb"] for r in records),
        "memory_stable_after_first_window": len(set(totals)) == len(set(allocations)) == 1,
    }


def verify_run(directory, reference, labels, simulation, fixed):
    log = (directory / "probe.log").read_text()
    code = int((directory / "exit_code.txt").read_text())
    require(code == 0, "Unexpected replay failure")
    count = int(re.search(r"host_gelu=std::erf samples=(\d+)", log)[1])
    mode = "hybrid" if "requested_backend=Alnpu,CpuAcc,CpuRef" in log else (
        "cpu_quantized" if "quantized_branches=2" in log else "baseline")
    require(count in {1, 10, 184}, "Unexpected validation scope")
    require("result=inference_completed samples=" + str(count) in log, "Missing completion marker")
    if fixed:
        require("runtime_policy=per_segment cma_policy=npu_segments_only" in log, "Unexpected runtime policy")
    raw = rows(directory / "predictions.csv")
    require(len(raw) == count and [int(r["sample_index"]) for r in raw] == list(range(count)),
            "Incomplete or reordered predictions")
    pred = np.array([float(r["prediction"]) for r in raw], np.float32)
    refs = np.array([float(r["reference"]) for r in raw], np.float32)
    target = np.array([float(r["target"]) for r in raw], np.float32)
    require(np.array_equal(refs, reference[:count]) and np.array_equal(target, labels[:count]),
            "Frozen validation reference/labels differ")
    require(np.isfinite(pred).all(), "Nonfinite predictions")
    delta = pred.astype(np.float64) - refs
    if mode == "baseline":
        require(np.all(np.abs(delta) <= 1e-4 + 1e-4 * np.abs(refs)), "FP32 reconstruction tolerance exceeded")
    npu_layers, npu_events = 0, 0
    for segment in range(6):
        dot = (directory / f"segment_{segment}.dot").read_text()
        npu_layers += dot.count("LayerType : ALHardNPU")
        profile = json.loads((directory / f"segment_{segment}_profile.json").read_text())
        npu_events += sum(len(times) for name, times in execution_events(profile) if "AlnpuALHardNPU" in name)
        expected = 1 if mode == "hybrid" and segment >= 4 else 0
        require(dot.count("LayerType : ALHardNPU") == expected, "Unexpected NPU segment assignment")
    require(npu_layers == npu_events == (2 if mode == "hybrid" else 0), "Graph/profile NPU count differs")
    require((directory / "runtime.sha256").read_text().split()[0] == RUNTIME, "Paired runtime differs")
    before, after = [interrupts(directory / f"interrupts_{tag}.txt") for tag in ["before", "after"]]
    irq = {key: after[key] - before[key] for key in before}
    require(irq["HARD_NPU_IRQ"] == (3 * count if mode == "hybrid" else 0) and irq["SOFT_NPU_IRQ"] == 0,
            "Unexpected physical interrupt delta")
    errors = pred.astype(np.float64) - target
    timing = np.array([float(r["forward_us"]) for r in raw])
    summary = {
        "mode": mode, "samples": count, "exit_code": code,
        "rmse": float(np.sqrt(np.mean(errors ** 2))), "mae": float(np.abs(errors).mean()),
        "fp32_reference_rmse": float(np.sqrt(np.mean(delta ** 2))),
        "fp32_reference_max_abs": float(np.abs(delta).max()),
        "pc_quantized_reference_max_abs": float(np.abs(pred.astype(np.float64) - simulation[:count]).max()),
        "mean_forward_us": float(timing.mean()), "timing_is_diagnostic": True,
        "NPU_graph_layers": npu_layers, "NPU_profiled_first_window_events": npu_events,
        "interrupt_delta": irq, "memory": memory_run(directory, count, mode == "hybrid", fixed),
        "application_accuracy_acceptance": "TBD",
    }
    return summary, pred


def verify(recovered):
    inventory = json.loads((ROOT / "results/board/cma-replay-v1-files-sha256.json").read_text())
    files = inventory["files"]
    require(len(files) == len(list(recovered.rglob("*"))) - len([p for p in recovered.rglob("*") if p.is_dir()]),
            "Recovered raw-file scope differs")
    for entry in files:
        path = recovered / entry["path"]
        require(path.stat().st_size == entry["bytes"] and hashlib.sha256(path.read_bytes()).hexdigest() == entry["sha256"],
                "Recovered raw bytes differ: " + entry["path"])
    with np.load(ROOT / "results/calibration/rul-uint8-v1/head-spline-validation.npz", allow_pickle=False) as snapshot:
        reference, labels, simulation = [snapshot[key].copy() for key in ["baseline", "labels", "prediction"]]
    reports, predictions = {}, {}
    for relative, fixed in [("cma-v1/f3p-cma-diagnostic-v1", False), ("cma-fixed-v2", True)]:
        for directory in sorted((recovered / relative).glob("results_*")):
            summary, pred = verify_run(directory, reference, labels, simulation, fixed)
            key = ("fixed" if fixed else "diagnostic") + "/" + summary["mode"] + "/" + str(summary["samples"])
            require(key not in reports, "Duplicate replay scope")
            reports[key], predictions[key] = summary, pred
    expected = {"diagnostic/hybrid/1", "diagnostic/hybrid/10", "fixed/hybrid/1", "fixed/hybrid/10",
                "fixed/hybrid/184", "fixed/baseline/184", "fixed/cpu_quantized/184"}
    require(set(reports) == expected, "Missing replay comparisons")
    for count in [1, 10]:
        require(np.array_equal(predictions[f"diagnostic/hybrid/{count}"], predictions[f"fixed/hybrid/{count}"]),
                "Runtime isolation changed diagnostic predictions")
        require(np.array_equal(predictions[f"fixed/hybrid/{count}"], predictions["fixed/hybrid/184"][:count]),
                "Repeated replay prediction prefix differs")
    rejected = list((recovered / "cma-fixed-v1").glob("results_*"))
    require(len(rejected) == 1 and int((rejected[0] / "exit_code.txt").read_text()) == 2
            and "already has an allocator associated with it" in (rejected[0] / "probe.log").read_text(),
            "Global allocator registration rejection differs")
    return {
        "status": "full_validation_hybrid_replay_and_cma_stability_verified",
        "raw_files_verified": len(files), "runs": reports,
        "full_validation_samples": 184, "hybrid_NPU_branches": 2,
        "per_window_CMA_growth_resolved_for_this_replay": True,
        "CMA_session_cleanup_required": True,
        "NPU_branches": ["output_layer.0.spline_weight", "output_layer.2.spline_weight"],
        "quantization_changed": False, "training_executed": False, "test_dataset_opened": False,
        "production_accuracy_accepted": False, "formal_warmed_performance_benchmark": False,
        "all_model_branches_on_NPU": False,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT / "results/board/recovered/cma-replay-v1")
    print(json.dumps(verify(parser.parse_args().root), ensure_ascii=False, indent=2))
