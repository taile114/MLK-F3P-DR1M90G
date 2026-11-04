"""Prepare frozen-weight hybrid RUL diagnostics on validation fixtures only."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import tarfile

import numpy as np
import torch
from calibrate_rul_uint8 import ROOT, CHECKPOINT_SHA256, digest, write_json
from fkp_dvgt.data import load_dataset
from fkp_dvgt.model import DVGTformer_model


def main():
    root = ROOT / "work/f3p-quantized-native-v1"
    root.mkdir(parents=True, exist_ok=False)
    for name in ["src", "bin", "data"]:
        (root / name).mkdir()
    checkpoint = ROOT / "training/outputs/motor_stable23_v1_gpu/best.pt"
    if digest(checkpoint) != CHECKPOINT_SHA256:
        raise ValueError("Unexpected checkpoint")
    torch.manual_seed(44)
    loaded = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model = DVGTformer_model(**loaded["config"]["model"])
    model.load_state_dict(loaded["model_state_dict"], strict=True)
    state = {k: v.detach().numpy() for k, v in model.state_dict().items()}
    state["__positional_encoding"] = model.positional_encoding.detach().numpy()
    offset = 0
    index = []
    with (root / "weights.bin").open("wb") as stream:
        for name, value in sorted(state.items()):
            stream.write(value.astype("<f4").tobytes())
            index.append(" ".join(map(str, [name, value.ndim, *value.shape, value.size, offset])))
            offset += value.size
    (root / "weights.tsv").write_text("\n".join(index) + "\n", encoding="ascii", newline="\n")
    with tarfile.open(ROOT / "artifacts/f3p-native-rul-arm64-v1.tar.gz") as archive:
        prefix = "f3p-native-rul-v1/"
        for name in ["weights.bin", "weights.tsv"]:
            import hashlib
            if digest(root / name) != hashlib.sha256(archive.extractfile(prefix + name).read()).hexdigest():
                raise ValueError("Native weight dependency differs")
        (root / "src/model_config.hpp").write_bytes(archive.extractfile(prefix + "src/model_config.hpp").read())
    with tarfile.open(ROOT / "artifacts/f3p-npu-fc-pad-arm64-v1.tar.gz") as archive:
        (root / "bin/timeout").write_bytes(archive.extractfile("f3p-npu-fc-pad-v1/bin/timeout").read())
    calibration_root = ROOT / "results/calibration/rul-uint8-v1"
    calibration = json.loads((calibration_root / "calibration.json").read_text())
    selected = ["output_layer.0.spline_weight", "output_layer.2.spline_weight"]
    with (root / "qparams_head.tsv").open("w", encoding="ascii", newline="\n") as stream:
        for name in selected:
            entry = calibration["layers"][name]
            stream.write(name + " " + " ".join(map(str, [entry[k] for k in ["input_features", "output_features", "input_scale", "weight_scale", "output_scale"]])) + "\n")
    val_path = ROOT / "training/processed/run_to_failure_5motors_v1/val.npz"
    x, y = load_dataset(val_path).tensors
    indices = [0, len(x) // 2, len(x) - 1]
    with np.load(calibration_root / "all-validation.npz", allow_pickle=False) as previous:
        baseline = previous["baseline"]
    x[indices].numpy().astype("<f4").tofile(root / "data/input_f32.bin")
    y[indices].numpy().astype("<f4").tofile(root / "data/labels_f32.bin")
    baseline[indices].astype("<f4").tofile(root / "data/reference_f32.bin")
    for source, target in [("native_rul_quantized.cpp", "src/native_rul_quantized.cpp"),
                           ("build_native_quantized.sh", "build.sh"), ("run_native_quantized.sh", "run.sh")]:
        shutil.copyfile(ROOT / "probe" / source, root / target)
    (root / "weights.sha256").write_text("".join(digest(root / name) + "  " + name + "\n" for name in ["weights.bin", "weights.tsv"]), encoding="ascii", newline="\n")
    write_json(root / "preparation.json", {"checkpoint_sha256": CHECKPOINT_SHA256,
        "validation_indices": indices, "validation_sha256": digest(val_path), "quantized_branches": selected,
        "calibration_sha256": digest(calibration_root / "calibration.json"),
        "weights_dependency": "Byte-identical weights from the verified native CPU baseline",
        "test_dataset_opened": False, "training_executed": False, "application_accuracy_threshold": "TBD"})
    source = root.with_name(root.name + ".source.tar.gz")
    if source.exists():
        raise FileExistsError(source)
    with tarfile.open(source, "w:gz") as archive:
        for path in sorted(root.rglob("*")):
            archive.add(path, arcname=(Path(root.name) / path.relative_to(root)).as_posix(), recursive=False)
    print(json.dumps({"source": str(source), "bytes": source.stat().st_size}))


if __name__ == "__main__":
    main()
