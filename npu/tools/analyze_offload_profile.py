"""Read an instrumented frozen-model replay; do not execute inference."""
from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path


def records(path, delimiter=","):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def analyze(directory):
    windows = records(directory / "predictions.csv")
    operations = records(directory / "operations.tsv", "\t")
    if len(operations) != 375 or not windows:
        raise ValueError("Unexpected graph or empty replay")
    if any(int(r["calls"]) != len(windows) for r in operations):
        raise ValueError("Incomplete operation measurements")
    if any(int(r["sample_index"]) != n for n, r in enumerate(windows)):
        raise ValueError("Nonsequential windows")
    total = sum(float(r["forward_us"]) for r in windows)
    by_kind = defaultdict(float)
    for r in operations:
        by_kind[r["kind"]] += float(r["total_us"])
    gelu = records(directory / "host_gelu.tsv", "\t")[0]
    if int(gelu["calls"]) != 5 * len(windows):
        raise ValueError("Unexpected GELU count")
    by_kind["host_exact_gelu"] = float(gelu["total_us"])
    measured = sum(by_kind.values())
    if measured > total * 1.001:
        raise ValueError("Measured nested time exceeds forward time")
    by_kind["feed_copy_dispatch_and_measurement"] = total - measured
    fc = [r for r in operations if r["kind"] == "fc"]
    if len(fc) != 124 or len({r["label"] for r in fc}) != 124:
        raise ValueError("Unexpected FC branch set")
    candidates = []
    for r in sorted(fc, key=lambda row: float(row["mean_us"]), reverse=True):
        rows, inner, units = (int(r[k]) for k in ["rows", "inner", "units"])
        candidates.append({"layer": r["label"], "rows": rows, "input_features": inner,
            "output_features": units, "logical_macs": rows*inner*units,
            "mean_operation_us": float(r["mean_us"]), "forward_fraction": float(r["total_us"])/total,
            "padded_input_features": (inner+7)//8*8, "padded_output_features": (units+7)//8*8})
    macs = sum(r["logical_macs"] for r in candidates)
    if macs != 3501280:
        raise ValueError(f"Unexpected FC arithmetic scope: {macs}")
    return {"status": "profile_verified", "samples": len(windows), "operations": len(operations),
        "mean_instrumented_forward_us": total/len(windows), "logical_fc_macs": macs,
        "timing_is_instrumented": True, "hardware_offload_verified": False,
        "categories": [{"kind": kind, "mean_us": us/len(windows), "forward_fraction": us/total}
                       for kind, us in sorted(by_kind.items(), key=lambda item: item[1], reverse=True)],
        "fc_candidates": candidates}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        if args.output.exists():
            raise FileExistsError(args.output)
        args.output.write_text(text, encoding="utf-8", newline="\n")
    print(json.dumps({k:v for k,v in result.items() if k != "fc_candidates"},ensure_ascii=False,indent=2))
    print(json.dumps(result["fc_candidates"][:12],ensure_ascii=False,indent=2))
