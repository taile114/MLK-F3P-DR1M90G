"""Prepare value-preserving native-FC padding controls; no trained weights used."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

NPU = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=NPU / "work/f3p-npu-fc-pad-v1")
    args = parser.parse_args()
    root = args.output.resolve()
    archive = root.with_name(root.name + ".source.tar.gz")
    if root.exists() or archive.exists():
        raise FileExistsError("Refusing to overwrite a control bundle")
    inventory = NPU / "results/build/rul-linear-shape-inventory.json"
    shapes = json.loads(inventory.read_text())["shape_controls_to_validate"]
    matrix = [(f"m{i:02}", s["rows"], s["input_features"], s["output_features"])
              for i, s in enumerate(shapes)]
    first = [row for row in matrix if row[1:] == (11, 12, 21)]
    attention = [row for row in matrix if row[3] == 21]
    if len(matrix) != 24 or len(first) != 1 or len(attention) != 4:
        raise ValueError("Unexpected dimension inventory")
    timeout = NPU / "work/f3p-timeout-compat-v1/timeout"
    timeout_report = json.loads((NPU / "results/build/timeout-compat-v1-verification.json").read_text())
    if timeout.exists():
        timeout_bytes = timeout.read_bytes()
    else:
        with tarfile.open(NPU / "artifacts/f3p-npu-fc-pad-arm64-v1.tar.gz") as saved:
            timeout_bytes = saved.extractfile("f3p-npu-fc-pad-v1/bin/timeout").read()
    if hashlib.sha256(timeout_bytes).hexdigest() != timeout_report["binary_sha256"]:
        raise ValueError("Timeout helper differs")
    (root / "src").mkdir(parents=True)
    (root / "bin").mkdir()
    for source, target in [("npu_linear_padded.cpp", "src/npu_linear_padded.cpp"),
                           ("build_npu_fc_pad.sh", "build.sh"), ("run_npu_fc_pad.sh", "run.sh"),
                           ("timeout_compat.c", "src/timeout_compat.c")]:
        shutil.copyfile(NPU / "probe" / source, root / target)
    (root / "bin/timeout").write_bytes(timeout_bytes)
    control = [("c00", 8, 8, 8)]
    for name, group in {"control": control, "first": first, "attention": attention, "matrix": matrix}.items():
        (root / (name + "_shapes.tsv")).write_text("# id rows input_features output_features\n" + "".join(
            "\t".join(map(str, row)) + "\n" for row in group), encoding="ascii", newline="\n")
    cases = [{"name": name + "_fc_pad8", "mode": "fc_pad8", "rows": b, "input_features": i,
              "output_features": o} for name, b, i, o in control + matrix]
    (root / "cpu_cases.tsv").write_text("".join(
        f"{c['name']}\t{c['rows']}\t{c['input_features']}\t{c['output_features']}\tfc_pad8\n" for c in cases),
        encoding="ascii", newline="\n")
    preparation = {"scope": "Native-FC zero-padding controls after unpadded m06 physical execution failure",
                   "expected_cpu_cases": len(cases), "cpu_cases": cases,
                   "shape_inventory_sha256": digest(inventory),
                   "sources": {name: digest(root / name) for name in
                               ["src/npu_linear_padded.cpp", "src/timeout_compat.c", "build.sh", "run.sh"]},
                   "timeout_sha256": hashlib.sha256(timeout_bytes).hexdigest(), "training_executed": False}
    (root / "preparation.json").write_text(json.dumps(preparation, indent=2) + "\n", encoding="utf-8", newline="\n")
    (root / "README.md").write_text('''# Independent padded native-FC control

The original B=11,I=12,O=21 native FC was accepted by optimization/loading
but physically timed out. Do not rerun that unpadded case on Alnpu.
Recover the hardware by a clean shutdown and full power cycle first.

This package keeps the ORIGINAL B/I/O for synthetic data and the integer
oracle, pads native FC input/output dimensions to multiples of eight, fills
added inputs/weights/biases with real zero, checks padded outputs, and crops
the restored result to the original output count. Zero is byte 128 in U8.
For m06, physical [11,16] x [24,16] weights yields original [11,21] after crop.
This is a hypothesis for a workaround, not an established hardware rule.
No trained weights, calibration data or training are used.

The tested timeout supervisor is INCLUDED in bin/timeout and the runner
prepends that directory to PATH. No separate installer or system changes.
For each shape, CpuRef runs before the single Alnpu backend. No CPU fallback.
The group only invokes fc_pad8, never the known-failing unpadded expression.

On the recovered board, verify checksums.sha256 and run control. Review that
result before first (m06 padded only), then attention (four output-21 shapes),
then matrix (all 24 shapes). Each command is: sh run.sh GROUP.
All unexpected failures stop the group. A driver interrupt timeout or CRASHED
message requires preserving logs and hardware recovery before further calls.

Original program and archive remain unchanged. Three profiled iterations,
no warm-up, diagnostic timings only. CpuRef is not the deployed mixed CPU
performance baseline. CPU/QEMU checks do not establish NPU compatibility.
Keep results on the SD data partition. Use gzip -dc ... | tar -xf -.
''', encoding="ascii", newline="\n")

    def metadata(member):
        member.mtime = 0
        member.uid = member.gid = 0
        member.uname = member.gname = ""
        member.mode = 0o755 if member.isdir() or member.name.endswith(".sh") or member.name.endswith("/bin/timeout") else 0o644
        return member

    with tarfile.open(archive, "w:gz") as bundle:
        bundle.add(root, arcname=root.name, filter=metadata)
    print(json.dumps({"source_archive": str(archive), "cpu_cases": len(cases), "groups": ["control", "first", "attention", "matrix"]}, indent=2))


if __name__ == "__main__":
    main()
