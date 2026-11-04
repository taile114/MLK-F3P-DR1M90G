"""Prepare synthetic UINT8 dimension controls; no model inference or training."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile

NPU = Path(__file__).resolve().parents[1]
MODES = ("fc", "conv", "conv_pad8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=NPU / "work/f3p-npu-linear-v1")
    args = parser.parse_args()
    root = args.output.resolve()
    archive = root.with_name(root.name + ".source.tar.gz")
    if root.exists() or archive.exists():
        raise FileExistsError("Refusing to overwrite a control bundle")
    inventory = NPU / "results/build/rul-linear-shape-inventory.json"
    report = json.loads(inventory.read_text(encoding="utf-8"))
    if digest(NPU / "probe/native_rul.cpp") != report["source_cpp_sha256"]:
        raise ValueError("Model graph differs from dimension inventory")
    shapes = report["shape_controls_to_validate"]
    if len(shapes) != report["unique_fc_shapes"] or len(shapes) != 24:
        raise ValueError("Unexpected dimension inventory")
    matrix = [(f"m{i:02}", s["rows"], s["input_features"], s["output_features"])
              for i, s in enumerate(shapes)]
    selected = {(110, 23, 23), (110, 207, 23), (11, 1260, 12),
                (1, 1188, 64), (1, 576, 1), (10, 207, 1)}
    smoke = [row for row in matrix if row[1:] in selected]
    if len(smoke) != len(selected):
        raise ValueError("Representative shapes missing")
    groups = {"control": [("c00", 8, 8, 8)], "smoke": smoke, "matrix": matrix}
    (root / "src").mkdir(parents=True)
    for source, target in [("npu_linear.cpp", "src/npu_linear.cpp"),
                           ("build_npu_linear.sh", "build.sh"), ("run_npu_linear.sh", "run.sh")]:
        shutil.copyfile(NPU / "probe" / source, root / target)
    for group, rows in groups.items():
        text = "# id rows input_features output_features\n"
        text += "".join("\t".join(map(str, row)) + "\n" for row in rows)
        (root / f"{group}_shapes.tsv").write_text(text, encoding="ascii", newline="\n")
    cases = []
    for group in ["control", "matrix"]:
        for name, rows, inputs, outputs in groups[group]:
            for mode in MODES[:2] if group == "control" else MODES:
                cases.append({"name": name + "_" + mode, "mode": mode, "rows": rows,
                              "input_features": inputs, "output_features": outputs})
    (root / "cpu_cases.tsv").write_text("".join(
        f"{c['name']}\t{c['rows']}\t{c['input_features']}\t{c['output_features']}\t{c['mode']}\n"
        for c in cases), encoding="ascii", newline="\n")
    (root / "README.md").write_text('''# F3P UINT8 linear-dimension controls

Synthetic controls use the 24 unique FC dimensions of the frozen native RUL
graph. No checkpoint, training dataset, calibration or training is used.

Mappings: fc is native FullyConnected with weights [O,I]. conv expresses the
same matrix as a 1x1 NCHW convolution [1,I,1,B]. conv_pad8 additionally pads
input/output channels to multiples of 8; padding is a hypothesis to test,
not an established hardware requirement. Real zero is encoded as byte 128.
All mappings use QAsymmU8 activations/weights/output, zero-points 128,
input scale .25, weight scale .125, Signed32 zero bias at scale .03125.
The output divisor is 8/16/32/64 for I <=64/256/512/2048 respectively.

Each process changes the input in all three iterations. A separate integer
matrix-product oracle checks all physical outputs, including padded ones,
with tolerance 1 LSB. Test ordering is CpuRef then Alnpu. Exactly one backend
is requested; optimized compute assignments are checked. No CPU fallback is
permitted for Alnpu. Graphs, profiles, element CSV, final physical output bytes,
dequantized original output, memory snapshots and IRQ snapshots are saved.

On F3P, from this extracted directory, use the paired SD runtime:

    sha256sum -c checksums.sha256
    sh run.sh control

After reviewing that result, continue with sh run.sh smoke (6 representative
model shapes), then sh run.sh matrix (all 24 shapes). Control uses an 8x8x8
matrix and tests native FC and 1x1 convolution. The matrix group contains
72 mapping cases; smoke is a subset of it.

Each case has a 30-second timeout. Only Alnpu optimize rejection (40) is
recorded as a capability restriction and allowed to continue. CPU failure,
load failure, numerical mismatch, crash or timeout stops the survey.
The final survey-completed message does not mean every mapping is supported.

Enqueue time and quantize/pack/enqueue/sync/restore time are reported separately.
The latter includes layout conversion and output dequantization, excluding
synthetic input generation, reference computation and file output. All three
iterations are profiled, with no warm-up: these are diagnostic times, not
steady-state latency or a full-RUL speedup measurement.

Use gzip -dc PACKAGE.tar.gz | tar -xf - on the paired BusyBox image.
Keep results on the SD second partition, not the small boot partition or
initramfs /root. Do not extract over existing results. Board execution is
separate from QEMU CPU prechecks; QEMU cannot validate physical NPU support.
''', encoding="ascii", newline="\n")
    preparation = {"scope": "Synthetic UINT8 dimension-control bundle", "training_executed": False,
                   "trained_model_weights_used": False, "shape_inventory_sha256": digest(inventory),
                   "groups": {key: [list(row) for row in value] for key, value in groups.items()},
                   "cpu_cases": cases,
                   "sources": {name: digest(root / name) for name in
                               ["src/npu_linear.cpp", "build.sh", "run.sh"]}}
    (root / "preparation.json").write_text(json.dumps(preparation, indent=2) + "\n",
                                          encoding="utf-8", newline="\n")

    def metadata(member):
        member.mtime = 0
        member.uid = member.gid = 0
        member.uname = member.gname = ""
        member.mode = 0o755 if member.isdir() or member.name.endswith(".sh") else 0o644
        return member

    with tarfile.open(archive, "w:gz") as bundle:
        bundle.add(root, arcname=root.name, filter=metadata)
    print(json.dumps({"directory": str(root), "source_archive": str(archive),
                      "unique_model_shapes": len(matrix), "cpu_cases": len(cases)}, indent=2))


if __name__ == "__main__":
    main()
