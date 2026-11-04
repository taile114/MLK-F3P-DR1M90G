"""Summarize optimized DOT assignments and optional first-window execution events."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re


def summarize(root):
    totals = Counter()
    compute_totals = Counter()
    segments = []
    structural = {"Input", "Output", "Constant", "MemCopy", "MemImport", "MemSync"}
    for i in range(6):
        path = root / f"segment_{i}.dot"
        rows = re.findall(r"LayerType : ([^\\]+)\\lBackendID : ([^\\]+)\\l", path.read_text())
        if not rows:
            raise ValueError("No backend assignments in " + str(path))
        counts = Counter(backend for kind, backend in rows)
        totals.update(counts)
        compute_counts = Counter(backend for kind, backend in rows if kind not in structural)
        compute_totals.update(compute_counts)
        kinds = defaultdict(Counter)
        for kind, backend in rows:
            kinds[backend][kind] += 1
        entry = {"segment": i, "graph_node_counts": dict(counts),
                 "compute_node_counts": dict(compute_counts),
                 "layer_types_by_backend": {key: dict(value) for key, value in kinds.items()}}
        profile_path = root / f"segment_{i}_profile.json"
        if profile_path.exists():
            names = []
            def walk(value):
                if isinstance(value, dict):
                    for key, child in value.items():
                        if any(part in key for part in ["Neon", "Ref", "CpuAcc", "CpuRef"]):
                            names.append(key)
                        walk(child)
                elif isinstance(value, list):
                    for child in value:
                        walk(child)
            walk(json.loads(profile_path.read_text()))
            executions = [name for name in dict.fromkeys(names)
                          if "Workload_Execute" in name and "Constant" not in name]
            events = defaultdict(list)
            for name in executions:
                backend = "CpuAcc" if name.startswith("Neon") else "CpuRef" if name.startswith("Ref") else "other"
                events[backend].append(name)
            entry["execution_event_counts"] = {key: len(value) for key, value in events.items()}
            entry["execution_profile_examples"] = {key: value[:6] for key, value in events.items()}
        segments.append(entry)
    return {"graph_node_counts": dict(totals), "compute_node_counts": dict(compute_totals),
            "note": "Node counts are not FLOP proportions or runtime speedup; graph assignment and execution profiling are separate evidence.",
            "segments": segments}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(summarize(args.results), indent=2) + "\n"
    if args.output:
        if args.output.exists():
            raise FileExistsError(args.output)
        args.output.write_text(result, encoding="utf-8", newline="\n")
    print(result, end="")


if __name__ == "__main__":
    main()
