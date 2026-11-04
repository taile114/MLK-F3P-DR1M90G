"""Independently check a frozen fault-test replay using only the standard library."""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import struct


def floats(path):
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError(f"Invalid float file: {path}")
    values = struct.unpack(f"<{len(data)//4}f", data)
    if not all(map(math.isfinite, values)):
        raise ValueError(f"Nonfinite data: {path}")
    return values


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode(raw, thresholds):
    c = max(range(5), key=lambda k: raw[k])
    g = 0 if c == 0 else 1 + sum(raw[4 + c] >= thresholds[2 * (c - 1) + j] for j in range(2))
    confidence = 1 / sum(math.exp(raw[k] - raw[c]) for k in range(5))
    return c, g, confidence


def percentile(values, p):
    values = sorted(values)
    at = (len(values) - 1) * p
    lo, hi = math.floor(at), math.ceil(at)
    return values[lo] + (values[hi] - values[lo]) * (at - lo)


def irq_counts(path, kind=None):
    counts = {}
    for line in path.read_text().splitlines():
        if not re.search(r"npu", line, re.I) or ":" not in line:
            continue
        if kind and not re.search(kind + r"[_ -]*npu", line, re.I):
            continue
        key, fields = line.split(":", 1)
        total = 0
        for word in fields.split():
            if not word.isdigit():
                break
            total += int(word)
        counts[key.strip()] = total
    return counts


def verify(bundle, result, execution):
    for line in (bundle / "checksums.sha256").read_text().splitlines():
        expected, name = line.split(None, 1)
        path = (bundle / name.lstrip(" *")).resolve()
        if not path.is_relative_to(bundle.resolve()) or sha(path) != expected:
            raise ValueError(f"Bundle hash mismatch: {name}")
    if (result / "exit_code.txt").read_text().strip() != "0":
        raise ValueError("Replay did not exit successfully; keep original logs")
    program_record = result / "program.sha256"
    if program_record.exists() and program_record.read_text().split()[0] != sha(bundle / "bin/fault_test"):
        raise ValueError("Replay used a different executable")
    if execution == "board":
        if not program_record.exists() or (result / "assets.sha256").read_bytes() != (bundle / "checksums.sha256").read_bytes():
            raise ValueError("Missing or inconsistent board program/assets identity")
    summary = json.loads((result / "summary.json").read_text())
    mode = summary["mode"]
    if mode not in ("cpu", "quantized", "armnn_ref", "hybrid", "hybrid_split"):
        raise ValueError("Unknown replay mode")
    with (result / "predictions.csv").open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    with (bundle / "data/test_index.csv").open(encoding="utf-8", newline="") as stream:
        targets = list(csv.DictReader(stream))
    raw = floats(result / "raw_outputs.bin")
    fp, ptq = floats(bundle / "data/test_reference.bin"), floats(bundle / "data/test_quantized.bin")
    thresholds = floats(bundle / "thresholds.bin")
    n = len(rows)
    if not 0 < n <= len(targets) or n != summary["samples"] or len(raw) != n * 9:
        raise ValueError("Incomplete or inconsistent replay")
    hardware = mode in ("hybrid", "hybrid_split")
    if summary["npu_execution_requested"] != hardware or summary["cpu_fallback_allowed"]:
        raise ValueError("Backend identity mismatch")
    if len(thresholds) != 8 or any(thresholds[k] >= thresholds[k + 1] for k in range(0, 8, 2)):
        raise ValueError("Invalid ordinal thresholds")
    confusion = [[0] * 5 for _ in range(5)]
    class_ok = joint_ok = grade_ok = fault_n = changed_class = changed_grade = 0
    maximum_fp = maximum_q = 0.
    timings, memory = [], []
    for i, (row, target) in enumerate(zip(rows, targets)):
        if int(row["sample"]) != i or int(target["sample"]) != i:
            raise ValueError("Missing, duplicated or out-of-order samples")
        output = raw[9 * i:9 * i + 9]
        c, g, confidence = decode(output, thresholds)
        if int(row["class_index"]) != c or int(row["grade"]) != g or abs(float(row["confidence"]) - confidence) > 1e-6:
            raise ValueError("Decode/CSV mismatch")
        if any(abs(float(row[f"raw{k}"]) - output[k]) > 1e-6 for k in range(9)):
            raise ValueError("Raw binary/CSV mismatch")
        tc, tg = int(target["class_target"]), int(target["grade_target"])
        confusion[tc][c] += 1
        class_ok += c == tc
        joint_ok += (c, g) == (tc, tg)
        fault_n += tc != 0
        grade_ok += tc != 0 and (c, g) == (tc, tg)
        rc, rg, _ = decode(fp[9 * i:9 * i + 9], thresholds)
        changed_class += c != rc
        changed_grade += g != rg
        for k, v in enumerate(output):
            a, b = fp[9 * i + k], ptq[9 * i + k]
            maximum_fp = max(maximum_fp, abs(v - a))
            maximum_q = max(maximum_q, abs(v - b))
            expected = a if mode == "cpu" else b
            if mode in ("cpu", "quantized") and abs(v - expected) > 1e-4 + 1e-4 * abs(expected):
                raise ValueError("CPU port exceeded frozen numeric tolerance")
        us = float(row["forward_us"])
        if not math.isfinite(us) or us <= 0:
            raise ValueError("Invalid inference timing")
        timings.append(us)
        memory.append(int(row["cma_bytes"]))
    cnn_lsb = None
    if mode != "cpu":
        actual = (result / "cnn_output_u8.bin").read_bytes()
        expected = (bundle / "data/test_cnn_u8.bin").read_bytes()[:n * 768]
        if len(actual) != len(expected):
            raise ValueError("Incomplete UINT8 CNN output")
        cnn_lsb = max(abs(a - b) for a, b in zip(actual, expected))
        if mode == "quantized" and cnn_lsb != 0:
            raise ValueError("CPU integer CNN differs from independent int64 reference")
    if len(set(memory)) != 1 or memory[0] != summary["steady_cma_bytes"] or summary["after_release_cma_bytes"] != 0:
        raise ValueError("Unstable or unreleased CMA")
    evidence = {"strict_graph": False, "hard_npu_profile": False, "npu_irq_increment": None,
                "hard_npu_irq_increment": None, "soft_npu_irq_increment": None, "runtime_hash_matched": False,
                "integer_convolution_verified": False}
    if hardware:
        graphs = list(result.glob("cnn_*.dot"))
        if len(graphs) != (1 if mode == "hybrid" else 4):
            raise ValueError("Missing optimized CNN graphs")
        for graph in graphs:
            text = graph.read_text()
            if "LayerType : ALHardNPU" not in text:
                raise ValueError("No hard NPU graph node")
            for line in text.splitlines():
                layer = re.search(r"LayerType : ([^\\]+)", line)
                backend = re.search(r"BackendID : ([^\\]+)", line)
                if layer and layer[1] not in ("Input", "Output", "Constant", "MemImport", "MemSync", "MemCopy"):
                    if not backend or backend[1] != "Alnpu" or layer[1] not in ("ALHardNPU", "ALSoftNPU"):
                        raise ValueError("CPU fallback in NPU graph")
        evidence["strict_graph"] = True
        profiles = list(result.glob("cnn_*_profile.json"))
        if len(profiles) != len(graphs):
            raise ValueError("Missing actual CNN execution profiles")
        for p in profiles:
            if not re.search(r"AlnpuALHardNPUWorkload_Execute", p.read_text(), re.I):
                raise ValueError("No actual hard NPU profile event")
        evidence["hard_npu_profile"] = True
        if execution == "board":
            before, after = irq_counts(result / "interrupts_before.txt"), irq_counts(result / "interrupts_after.txt")
            delta = sum(after.get(k, 0) - v for k, v in before.items())
            if not before or set(before) != set(after) or delta <= 0:
                raise ValueError("No NPU IRQ increment")
            evidence["npu_irq_increment"] = delta
            for kind in ("hard", "soft"):
                initial = irq_counts(result / "interrupts_before.txt", kind)
                final = irq_counts(result / "interrupts_after.txt", kind)
                increment = sum(final.get(k, 0) - value for k, value in initial.items())
                if set(initial) != set(final) or increment < 0 or (kind == "hard" and (not initial or increment <= 0)):
                    raise ValueError("No valid hard NPU IRQ increment" if kind == "hard" else "Invalid soft NPU IRQ counters")
                evidence[kind + "_npu_irq_increment"] = increment
            expected = (bundle / "vendor-runtime.sha256").read_text().split()[0]
            actual = (result / "runtime.sha256").read_text().split()[0]
            if actual != expected:
                raise ValueError("Board Arm NN differs from the fixed build runtime")
            evidence["runtime_hash_matched"] = True
            new_log = (result / "dmesg_after.txt").read_text()
            old_log = (result / "dmesg_before.txt").read_text()
            new_lines = [line for line in new_log.splitlines() if line not in set(old_log.splitlines())]
            if any(re.search(r"(?:npu.*(?:timeout|failed|error)|cma.*fail)", line, re.I) for line in new_lines):
                raise ValueError("New NPU/CMA kernel error")
        if mode == "hybrid_split":
            audit = json.loads((result / "integer_audit.json").read_text())
            required = {i for i in (0, 84, 150, 200, 246, 300, 359) if i < n}
            if (audit["maximum_error_lsb"] > 1 or not audit["actual_input_used"]
                    or audit["integer_accumulator"] != "int64" or audit["scales_fitted_again"]
                    or audit["weights_sha256"] != sha(bundle / "weights.bin")
                    or audit["qparams_sha256"] != sha(bundle / "qparams.tsv")
                    or {(c["sample"], c["layer"]) for c in audit["checks"]} != {(i, k) for i in required for k in range(4)}):
                raise ValueError("Invalid or failed hard-NPU integer audit")
            for check in audit["checks"]:
                prefix = f"audit_cnn_{check['layer']}_{check['layer']+1}_sample{check['sample']}"
                if sha(result / (prefix + "_input.bin")) != check["input_sha256"] or sha(result / (prefix + "_output.bin")) != check["output_sha256"]:
                    raise ValueError("Changed hard-NPU audit capture")
            evidence["integer_convolution_verified"] = True
    report = {"schema": "mlk.fault.offline_verification.v1", "execution": execution, "mode": mode,
              "samples": n, "full_test_completed": n == len(targets),
              "class_correct": class_ok, "class_accuracy": class_ok / n,
              "class_and_grade_correct": joint_ok, "class_and_grade_accuracy": joint_ok / n,
              "fault_class_and_grade_correct": grade_ok, "fault_samples": fault_n,
              "fault_class_and_grade_accuracy": grade_ok / fault_n if fault_n else None,
              "confusion_matrix": confusion, "class_changes_vs_fp32": changed_class,
              "grade_changes_vs_fp32": changed_grade, "max_fp32_raw_error": maximum_fp,
              "max_quantized_raw_error": maximum_q, "max_cnn_lsb": cnn_lsb,
              "forward_us": {"mean": statistics.mean(timings), "p50": percentile(timings, .5), "p95": percentile(timings, .95), "max": max(timings)},
              "steady_cma_bytes": memory[0], "after_release_cma_bytes": 0,
              "npu_evidence": evidence,
              "numeric_cpu_reference_verified": mode in ("cpu", "quantized"),
              "physical_npu_execution_verified": execution == "board" and hardware,
              "independent_accuracy_acceptance": False, "hardware_speedup_verified": False,
              "result_files_sha256": {p.name: sha(p) for p in result.iterdir() if p.is_file() and p.name != "verification.json"}}
    (result / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "result_files_sha256"}, indent=2))
    return report


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bundle", type=Path, required=True)
    p.add_argument("--results", type=Path, required=True)
    p.add_argument("--execution", choices=("board", "emulation"), required=True)
    a = p.parse_args()
    verify(a.bundle, a.results, a.execution)
