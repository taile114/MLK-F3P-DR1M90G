"""Package all validation replay arrays, without reading the held-out test set."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import tarfile

import numpy as np
from calibrate_rul_uint8 import ROOT
from fkp_dvgt.data import load_dataset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "work/quantized-native-val184")
    args = parser.parse_args()
    out = args.output.resolve()
    archive = out.with_suffix(".tar.gz")
    if out.exists() or archive.exists():
        raise FileExistsError(out)
    x, y = load_dataset(ROOT / "training/processed/run_to_failure_5motors_v1/val.npz").tensors
    with np.load(ROOT / "results/calibration/rul-uint8-v1/all-validation.npz", allow_pickle=False) as previous:
        reference = previous["baseline"].copy()
    if len(x) != len(y) or len(x) != len(reference) or len(x) != 184:
        raise ValueError("Validation replay count differs")
    out.mkdir(parents=True)
    for name, array in [("input_f32.bin", x.numpy()), ("labels_f32.bin", y.numpy()), ("reference_f32.bin", reference)]:
        array.astype("<f4").tofile(out / name)
    (out / "checksums.sha256").write_text("".join(hashlib.sha256((out / name).read_bytes()).hexdigest() + "  " + name + "\n"
        for name in ["input_f32.bin", "labels_f32.bin", "reference_f32.bin"]), encoding="ascii", newline="\n")
    with tarfile.open(archive, "w:gz") as stream:
        for path in out.iterdir():
            stream.add(path, arcname="data/" + path.name)
    print(archive)


if __name__ == "__main__":
    main()
