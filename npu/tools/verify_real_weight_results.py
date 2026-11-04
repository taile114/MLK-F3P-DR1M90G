"""Read-only independent checks of real-weight UINT8 board output files."""
from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from verify_board_linear import execution_events, interrupts, require

ROOT = Path(__file__).resolve().parents[1]


def verify(bundle, recovered):
    preparation = json.loads((bundle / "preparation.json").read_text())
    cases = preparation["cases"]
    groups = {}
    for directory in sorted(recovered.glob("results_*")):
        with (directory / "summary.tsv").open(newline="") as stream:
            summary = list(csv.DictReader(stream, delimiter="\t"))
        reports = []
        for row in summary:
            name, backend = row["case"], row["backend"]
            require(int(row["exit_code"]) == 0, "Real-weight backend failed")
            folder = directory / (name + "_" + backend)
            require(int((folder / "exit_code.txt").read_text()) == 0, "Saved exit code differs")
            e = cases[name]
            b, i, o = e["rows"], e["physical_input_features"], e["physical_output_features"]
            source = bundle / "cases" / name
            x = np.fromfile(source / "inputs_u8.bin", np.uint8).reshape(3, b, i).astype(np.int64) - 128
            w = np.fromfile(source / "weights_u8.bin", np.uint8).reshape(o, i).astype(np.int64) - 128
            accumulator = x @ w.T
            value = accumulator.astype(np.float64) * (e["input_scale"] * e["weight_scale"] / e["output_scale"])
            expected = np.clip(np.sign(value) * np.floor(np.abs(value) + 0.5) + 128, 0, 255).astype(np.uint8)
            require(np.array_equal(expected.ravel(), np.fromfile(source / "expected_u8.bin", np.uint8)), "Fixture integer oracle differs")
            actual = np.fromfile(folder / "output_u8.bin", np.uint8).reshape(3, b, o)
            errors = np.abs(actual.astype(np.int16) - expected.astype(np.int16))
            require(errors.max() <= 1, "Board integer tolerance exceeded")
            with (folder / "output.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            require(len(rows) == actual.size, "Real-weight CSV count differs")
            for index, saved in enumerate(rows):
                sample, position = divmod(index, b * o)
                br, channel = divmod(position, o)
                require((int(saved["sample"]), int(saved["row"]), int(saved["channel"])) == (sample, br, channel)
                        and int(saved["actual_u8"]) == int(actual.ravel()[index])
                        and int(saved["expected_u8"]) == int(expected.ravel()[index])
                        and int(saved["error_lsb"]) == int(errors.ravel()[index]), "CSV/binary output differs")
            dot = (folder / "graph.dot").read_text()
            compute = "ALHardNPU" if backend == "Alnpu" else "FullyConnected"
            require("LayerType : " + compute in dot and "BackendID : " + backend in dot, "Unexpected real-weight graph")
            if backend == "Alnpu":
                require("BackendID : CpuRef" not in dot and "BackendID : CpuAcc" not in dot, "Unexpected CPU fallback")
            events = execution_events(json.loads((folder / "profile.json").read_text()))
            require(len(events) == 1 and compute + "Workload_Execute" in events[0][0] and len(events[0][1]) == 3,
                    "Missing real-weight workload executions")
            ref = np.fromfile(source / "reference_f32.bin", "<f4").reshape(3, b, e["output_features"])
            restored = (actual[:, :, :e["output_features"]].astype(np.float64) - 128) * e["output_scale"]
            reports.append({"case": name, "layer": e["layer"], "backend": backend,
                            "checked_outputs": int(actual.size), "max_error_lsb": int(errors.max()),
                            "fp32_branch_rmse": float(np.sqrt(np.mean((restored - ref) ** 2))),
                            "fp32_branch_max_abs": float(np.max(np.abs(restored - ref)))})
        before, after = (interrupts(directory / ("interrupts_" + tag + ".txt")) for tag in ["before", "after"])
        groups[directory.name] = {"runs": reports,
            "interrupt_delta": {name: after[name] - before[name] for name in before}}
    all_group = next(value for name, value in groups.items() if name.startswith("results_all_"))
    require(len(all_group["runs"]) == 12, "Expected six paired real-weight cases")
    require(all_group["interrupt_delta"]["HARD_NPU_IRQ"] > 0, "No hardware IRQ increase")
    return {"status": "passed", "scope": "Recovered real-weight branch files; not complete-model NPU acceptance",
            "checkpoint_sha256": preparation["checkpoint_sha256"], "groups": groups,
            "real_weight_npu_cases": 6, "train_only_calibration": True,
            "test_dataset_opened": False, "training_executed": False, "full_rul_npu_verified": False}


if __name__ == "__main__":
    print(json.dumps(verify(Path(sys.argv[1]), Path(sys.argv[2])), indent=2))
