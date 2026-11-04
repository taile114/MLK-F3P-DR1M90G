"""Independently verify saved synthetic linear CPU controls; never run a model."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(root):
    preparation = json.loads((root / "preparation.json").read_text())
    require(len(preparation["cpu_cases"]) == preparation.get("expected_cpu_cases", 74),
            "Unexpected number of CPU mapping controls")
    for name, expected in preparation["sources"].items():
        require(digest(root / name) == expected, "Source differs: " + name)
    binary = (root / "bin/npu_linear").read_bytes()
    require(binary[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", binary, 18)[0] == 183,
            "Expected little-endian AArch64 ELF64")
    require(int((root / "unexpected_failures.txt").read_text()) == 0, "CPU prechecks failed")
    runtime = (root / "runtime_hashes.txt").read_text().split()[0]
    require(runtime == "5def7ba75e4b59644be6f58deaf8b2bb3791c62ed38b52b6aea08b11a8f728ce",
            "Unexpected paired runtime")
    reports = []
    checked = 0
    for case in preparation["cpu_cases"]:
        folder = root / "emulation" / case["name"]
        require(int((folder / "exit_code.txt").read_text()) == 0, "Nonzero CPU exit: " + case["name"])
        rows, inputs, outputs = case["rows"], case["input_features"], case["output_features"]
        padded = case["mode"] in {"conv_pad8", "fc_pad8"}
        oc = (outputs + 7) // 8 * 8 if padded else outputs
        ic = (inputs + 7) // 8 * 8 if padded else inputs
        divisor = 8 if inputs <= 64 else 16 if inputs <= 256 else 32 if inputs <= 512 else 64
        weights = [[(o * 11 + i * 5 + (i // 13) * 2) % 7 - 3 for i in range(inputs)] for o in range(outputs)]
        expected = []
        for iteration in range(3):
            for row in range(rows):
                values = [(iteration * 11 + row * 13 + i * 7 + (i // 17) * 3) % 17 - 8 for i in range(inputs)]
                for o in range(oc):
                    accumulator = sum(x * w for x, w in zip(values, weights[o])) if o < outputs else 0
                    rounded = (abs(accumulator) + divisor // 2) // divisor
                    if accumulator < 0:
                        rounded = -rounded
                    expected.append(max(0, min(255, rounded + 128)))
        with (folder / "output.csv").open(newline="") as stream:
            actual = list(csv.DictReader(stream))
        require(len(actual) == len(expected) == 3 * rows * oc, "Incorrect output count: " + case["name"])
        errors = []
        for index, record in enumerate(actual):
            iteration, position = divmod(index, rows * oc)
            row, o = divmod(position, oc)
            require((int(record["iteration"]), int(record["row"]), int(record["output_feature"])) == (iteration, row, o),
                    "Output order differs")
            require(int(record["is_padding"]) == int(o >= outputs), "Incorrect padding marker")
            require(int(record["expected_quantized"]) == expected[index], "Integer oracle differs")
            error = abs(int(record["actual_quantized"]) - expected[index])
            require(error == int(record["error_lsb"]) and error <= 1, "CPU numerical tolerance exceeded")
            errors.append(error)
        final = actual[-rows * oc:]
        raw = (folder / "output_bytes.bin").read_bytes()
        require(len(raw) == rows * oc, "Invalid physical output length")
        for record in final:
            row, o = int(record["row"]), int(record["output_feature"])
            index = row * oc + o if case["mode"] in {"fc", "fc_pad8"} else o * rows + row
            require(raw[index] == int(record["actual_quantized"]), "Physical output layout differs")
        restored = (folder / "dequantized_output_f32.bin").read_bytes()
        require(len(restored) == rows * outputs * 4, "Invalid restored output length")
        for row in range(rows):
            for o in range(outputs):
                actual_float = struct.unpack_from("<f", restored, 4 * (row * outputs + o))[0]
                expected_float = (int(final[row * oc + o]["actual_quantized"]) - 128) * divisor / 32
                require(actual_float == expected_float, "Restored output differs")
        dot = (folder / "graph.dot").read_text()
        require("BackendID : CpuRef" in dot and "BackendID : Alnpu" not in dot, "Unexpected CPU graph backend")
        json.loads((folder / "profile.json").read_text())
        log = (folder / "probe.log").read_text()
        require("result=passed" in log and "mismatches=0" in log, "Missing CPU completion")
        reports.append({**case, "physical_input_features": ic, "physical_output_features": oc,
                        "checked_outputs": len(actual), "max_error_lsb": max(errors), "exit_code": 0,
                        "output_csv_sha256": digest(folder / "output.csv")})
        checked += len(actual)
    return {"scope": "Independent integer-oracle and layout verification of saved QEMU CpuRef controls",
            "status": "passed", "cases": reports, "cpu_controls": len(reports), "checked_outputs": checked,
            "tolerance_lsb": 1, "paired_runtime_sha256": runtime,
            "binary_sha256": digest(root / "bin/npu_linear"), "sources": preparation["sources"],
            "shape_inventory_sha256": preparation["shape_inventory_sha256"],
            "training_executed": False, "trained_model_weights_used": False,
            "physical_linear_npu_execution_verified": False, "full_rul_npu_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        raise FileExistsError(args.output)
    result = verify(args.bundle.resolve())
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({key: value for key, value in result.items() if key != "cases"}, indent=2))


if __name__ == "__main__":
    main()
