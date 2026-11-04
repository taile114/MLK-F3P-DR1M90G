"""Verify recovered F3P linear probes from saved files; never execute inference."""
from __future__ import annotations

from collections import Counter
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = "5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fields(line):
    return dict(word.split("=", 1) for word in line.split() if "=" in word)


def execution_events(value):
    found = []
    if isinstance(value, dict):
        for key, child in value.items():
            if "Workload_Execute" in key and "Constant" not in key:
                times = [item["raw"] for name, item in child.items()
                         if name.startswith("Wall clock time_") and isinstance(item, dict)]
                require(len(times) == 1, "Missing workload duration")
                found.append((key, times[0]))
            found.extend(execution_events(child))
    elif isinstance(value, list):
        for child in value:
            found.extend(execution_events(child))
    return found


def verify_run(folder, mode, backend, rows, inputs, outputs):
    log = (folder / "probe.log").read_text()
    header = fields(next(line for line in log.splitlines() if line.startswith("mode=")))
    require(header["requested_backend"] == backend and header["cpu_fallback_allowed"] == "false"
            and header["trained_model_weights_used"] == "false", "Backend/model scope differs")
    padded = mode in {"conv_pad8", "fc_pad8"}
    ic = (inputs + 7) // 8 * 8 if padded else inputs
    oc = (outputs + 7) // 8 * 8 if padded else outputs
    require(all(int(header[key]) == value for key, value in {
        "rows": rows, "input_features": inputs, "output_features": outputs,
        "physical_input_features": ic, "physical_output_features": oc}.items()), "Probe dimensions differ")
    divisor = 8 if inputs <= 64 else 16 if inputs <= 256 else 32 if inputs <= 512 else 64
    weights = [[(o * 11 + i * 5 + (i // 13) * 2) % 7 - 3 for i in range(inputs)] for o in range(outputs)]
    expected = []
    for iteration in range(3):
        for row in range(rows):
            values = [(iteration * 11 + row * 13 + i * 7 + (i // 17) * 3) % 17 - 8 for i in range(inputs)]
            for o in range(oc):
                accumulator = sum(x * w for x, w in zip(values, weights[o])) if o < outputs else 0
                rounded = (abs(accumulator) + divisor // 2) // divisor
                rounded = -rounded if accumulator < 0 else rounded
                expected.append(max(0, min(255, rounded + 128)))
    with (folder / "output.csv").open(newline="") as stream:
        actual = list(csv.DictReader(stream))
    require(len(actual) == len(expected) == 3 * rows * oc, "Incorrect CSV length")
    errors = []
    for index, record in enumerate(actual):
        iteration, position = divmod(index, rows * oc)
        row, o = divmod(position, oc)
        require((int(record["iteration"]), int(record["row"]), int(record["output_feature"])) == (iteration, row, o)
                and int(record["is_padding"]) == int(o >= outputs)
                and int(record["expected_quantized"]) == expected[index], "CSV oracle/order differs")
        error = abs(int(record["actual_quantized"]) - expected[index])
        require(error == int(record["error_lsb"]) and error <= 1, "Integer tolerance exceeded")
        errors.append(error)
    raw = (folder / "output_bytes.bin").read_bytes()
    restored = (folder / "dequantized_output_f32.bin").read_bytes()
    require(len(raw) == rows * oc and len(restored) == rows * outputs * 4, "Output binary size differs")
    final = actual[-rows * oc:]
    for row in range(rows):
        for o in range(oc):
            index = row * oc + o if mode in {"fc", "fc_pad8"} else o * rows + row
            value = int(final[row * oc + o]["actual_quantized"])
            require(raw[index] == value, "Physical output layout differs")
            if o < outputs:
                real = struct.unpack_from("<f", restored, 4 * (row * outputs + o))[0]
                require(real == (value - 128) * divisor / 32, "Restored output differs")
    dot = (folder / "graph.dot").read_text()
    compute = "ALHardNPU" if backend == "Alnpu" else "FullyConnected" if mode in {"fc", "fc_pad8"} else "Convolution2d"
    require("LayerType : " + compute in dot and "BackendID : " + backend in dot,
            "Expected compute/backend missing in graph")
    if backend == "Alnpu":
        require("BackendID : CpuRef" not in dot and "BackendID : CpuAcc" not in dot, "Unexpected CPU fallback")
    events = execution_events(load(folder / "profile.json"))
    require(len(events) == 1 and compute + "Workload_Execute" in events[0][0]
            and len(events[0][1]) == 3 and all(math.isfinite(t) and t > 0 for t in events[0][1]),
            "Expected three profiled compute executions")
    result = fields(next(line for line in log.splitlines() if line.startswith("result=passed")))
    require(int(result["max_error_lsb"]) == max(errors) and result["mismatches"] == "0"
            and result["iterations"] == "3" and result["profiling_enabled"] == "true"
            and result["timing_is_diagnostic"] == "true", "Saved log/CSV result differs")
    return {"backend": backend, "rows": rows, "input_features": inputs, "output_features": outputs,
            "physical_input_features": ic, "physical_output_features": oc,
            "compute_layer_type": compute, "checked_outputs": len(actual), "max_error_lsb": max(errors),
            "mean_enqueue_us": float(result["mean_enqueue_us"]),
            "mean_quantize_pack_enqueue_sync_restore_us": float(result["mean_quantize_pack_enqueue_sync_restore_us"])}


def interrupts(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.endswith(("HARD_NPU_IRQ", "SOFT_NPU_IRQ")):
            parts = line.split()
            # Use only per-CPU counters, excluding the numeric GIC interrupt number.
            counters = line.split(":", 1)[1].split("GIC", 1)[0].split()
            result[parts[-1]] = sum(map(int, counters))
    require(set(result) == {"HARD_NPU_IRQ", "SOFT_NPU_IRQ"}, "Missing NPU interrupt counters")
    return result


def verify():
    inventory = load(ROOT / "results/board/linear-v1-recovered-files-sha256.json")
    require(inventory["file_count"] == len(inventory["files"]) == 1356, "Recovered file count differs")
    for entry in inventory["files"]:
        path = ROOT / entry["path"]
        require(path.resolve().is_relative_to((ROOT / "results/board/recovered/linear-v1").resolve()),
                "Unsafe recovered linear path")
        require(path.stat().st_size == entry["bytes"] and digest(path) == entry["sha256"],
                "Recovered file differs: " + entry["path"])
    expected_paths = {(ROOT / entry["path"]).resolve() for entry in inventory["files"]}
    actual_paths = {path.resolve() for path in (ROOT / "results/board/recovered/linear-v1").rglob("*") if path.is_file()}
    require(expected_paths == actual_paths, "Unexpected or missing recovered linear file")
    shapes = load(ROOT / "results/build/rul-linear-shape-inventory.json")["shape_controls_to_validate"]
    dimensions = {f"m{i:02}": (s["rows"], s["input_features"], s["output_features"]) for i, s in enumerate(shapes)}
    dimensions["c00"] = (8, 8, 8)
    base = ROOT / "results/board/recovered/linear-v1"
    groups = {}
    for category in ["original", "padded"]:
        for directory in sorted((base / category).glob("results_*")):
            require((directory / "runtime.sha256").read_text().split()[0] == RUNTIME, "Paired runtime differs")
            with (directory / "summary.tsv").open(newline="") as stream:
                summary = list(csv.DictReader(stream, delimiter="\t"))
            counts, runs = Counter(), []
            for row in summary:
                backend, mode, shape, code = row["backend"], row["mode"], row["shape"], int(row["exit_code"])
                folder = directory / (shape + "_" + mode) / backend
                require(int((folder / "exit_code.txt").read_text()) == code, "Saved exit code differs")
                counts[str(code)] += 1
                if code == 0:
                    runs.append({"shape": shape, "mode": mode,
                                 **verify_run(folder, mode, backend, *dimensions[shape])})
                elif code == 40:
                    require(category == "original" and backend == "Alnpu"
                            and "result=failed stage=optimize" in (folder / "probe.log").read_text(),
                            "Unexpected optimization rejection")
                else:
                    log = (folder / "probe.log").read_text()
                    require(category == "original" and shape == "m06" and mode == "fc" and backend == "Alnpu"
                            and code == 60 and "stage=load passed" in log and "filter_OC = 21" in log
                            and "CRASHED" in log and "stage=inference" in log, "Unexpected failure scope")
            before, after = (interrupts(directory / ("interrupts_" + tag + ".txt")) for tag in ["before", "after"])
            groups[category + "/" + directory.name] = {
                "exit_code_counts": dict(counts), "successful_runs": runs,
                "interrupt_delta": {name: after[name] - before[name] for name in before}}
    matrix = groups["padded/results_matrix_19700101_001312_453"]
    require(matrix["exit_code_counts"] == {"0": 48}
            and matrix["interrupt_delta"] == {"HARD_NPU_IRQ": 1287, "SOFT_NPU_IRQ": 0}, "Padded matrix differs")
    npu_runs = [run for run in matrix["successful_runs"] if run["backend"] == "Alnpu"]
    serial_matrix = load(ROOT / "results/board/npu-fc-pad-v1-matrix-result.json")
    for actual, transcribed in zip(npu_runs, serial_matrix["runs"], strict=True):
        require(actual["shape"] == transcribed["shape"]
                and actual["mean_enqueue_us"] == transcribed["npu_enqueue_us"]
                and actual["mean_quantize_pack_enqueue_sync_restore_us"] == transcribed["npu_total_us"],
                "Matrix serial transcription differs from original file")
    return {"scope": "Read-only numerical, graph, execution-profile and IRQ verification of recovered F3P files",
            "status": "passed", "files_verified": len(inventory["files"]), "groups": groups,
            "all_24_padded_fc_shapes_verified_from_files": True,
            "padded_matrix_npu_checked_outputs": sum(run["checked_outputs"] for run in npu_runs),
            "padded_matrix_max_error_lsb": max(run["max_error_lsb"] for run in npu_runs),
            "trained_model_weights_used": False, "training_executed": False,
            "inference_executed_by_verifier": False, "full_rul_npu_verified": False}


if __name__ == "__main__":
    print(json.dumps(verify(), ensure_ascii=False, indent=2))
