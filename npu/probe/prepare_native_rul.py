"""Prepare read-only checkpoint weights and replay fixtures for native inference."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tarfile

import numpy as np
import torch

HERE = Path(__file__).resolve().parent
NPU_ROOT = HERE.parent
MODEL_ROOT = NPU_ROOT / "training"
sys.path.insert(0, str(MODEL_ROOT))
from fkp_dvgt.model import DVGTformer_model


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=NPU_ROOT / "work/f3p-native-rul-v2")
    parser.add_argument("--replay-archive", type=Path,
                        default=NPU_ROOT / "artifacts/f3p-rul-probe-arm64.tar.gz")
    args = parser.parse_args()
    root = args.output.resolve()
    archive = root.with_suffix(".source.tar.gz")
    if root.exists() or archive.exists():
        raise SystemExit("Refusing to overwrite an existing native model package")
    checkpoint = MODEL_ROOT / "outputs/motor_stable23_v1_gpu/best.pt"
    expected = "bb7cb39da623e95574e57a559f1df979e5ee5ddea75d71d079f818da108d16f2"
    if digest(checkpoint) != expected:
        raise SystemExit("The trained checkpoint differs from the verified deployment baseline")
    torch.set_num_threads(1)
    loaded = torch.load(checkpoint, map_location="cpu", weights_only=True)
    config = loaded["config"]["model"]
    if (config["num_nodes"], config["time_length"], config["num_blocks"]) != (11, 10, 2):
        raise SystemExit("Native staging currently requires the verified 11-sensor, 10-step, 2-block model")
    model = DVGTformer_model(**config)
    model.load_state_dict(loaded["model_state_dict"], strict=True)
    model.eval()
    state = {k: v.detach().cpu().contiguous().numpy() for k, v in model.state_dict().items()}
    state["__positional_encoding"] = model.positional_encoding.detach().cpu().contiguous().numpy()
    for name, value in state.items():
        if value.dtype != np.float32 or not np.isfinite(value).all():
            raise ValueError("Expected finite Float32 state: " + name)
        if name.endswith(".grid") and (value.ndim != 2 or value.shape[1] != 13):
            raise ValueError("Unexpected KAN grid: " + name)
    (root / "src").mkdir(parents=True)
    (root / "data").mkdir()
    (root / "pc_reference").mkdir()
    entries = []
    offset = 0
    with (root / "weights.bin").open("wb") as stream:
        for name, value in sorted(state.items()):
            stream.write(value.astype("<f4", copy=False).tobytes())
            fields = [name, value.ndim, *value.shape, value.size, offset]
            entries.append(" ".join(map(str, fields)) + "\n")
            offset += value.size
    (root / "weights.tsv").write_text("".join(entries), encoding="ascii", newline="\n")
    header = """#pragma once
namespace native_config {
constexpr unsigned kHeads = %d;
constexpr unsigned kTemporalDim = %d;
constexpr unsigned kSpatialDim = %d;
constexpr unsigned kTemporalFF = %d;
constexpr unsigned kSpatialFF = %d;
constexpr float kLambda = %.9ef;
constexpr float kOneMinusLambda = %.9ef;
}
""" % (config["num_heads"], *config["d_model"], *config["d_ff"],
        np.float32(config["lambda_param"]), np.float32(1 - config["lambda_param"]))
    (root / "src/model_config.hpp").write_text(header, encoding="ascii", newline="\n")
    shutil.copyfile(HERE / "native_rul.cpp", root / "src/native_rul.cpp")
    shutil.copyfile(HERE / "run_native_rul.sh", root / "run_native_rul.sh")
    shutil.copyfile(HERE / "build_native_rul.sh", root / "build.sh")
    # Read only named members; retain the verified replay layout and contracts.
    with tarfile.open(args.replay_archive, "r:gz") as replay:
        for group, names in {
            "data": ["input_f32.bin", "reference_f32.bin", "labels_f32.bin", "samples.csv"],
            "contracts": ["input_contract.json", "normalization.npz", "preprocessing.json", "replay.json"],
        }.items():
            (root / group).mkdir(exist_ok=True)
            for name in names:
                member = replay.getmember("f3p-rul-probe/" + group + "/" + name)
                if not member.isfile():
                    raise ValueError("Expected regular replay file: " + member.name)
                (root / group / name).write_bytes(replay.extractfile(member).read())
    windows = np.fromfile(root / "data/input_f32.bin", dtype="<f4").reshape(-1, 23, 110)
    references = np.fromfile(root / "data/reference_f32.bin", dtype="<f4")
    if len(windows) != 165 or len(references) != len(windows):
        raise ValueError("Expected 165 verified replay windows")
    captured = []
    def capture(_, inputs):
        captured.append(inputs[0].detach().cpu().contiguous().numpy().copy())
    modules = [model.tvgtformer_blocks[0].feed_forward_temp[1], model.svgtformer_blocks[0].feed_forward_spat[1],
               model.tvgtformer_blocks[1].feed_forward_temp[1], model.svgtformer_blocks[1].feed_forward_spat[1],
               model.output_layer[1]]
    handles = [module.register_forward_pre_hook(capture) for module in modules]
    with torch.inference_mode():
        first = model(torch.from_numpy(windows[:1].copy()), graph_probability=0.0).item()
    for handle in handles:
        handle.remove()
    if len(captured) != 5 or abs(first - references[0]) > 1e-4 + 1e-4 * abs(references[0]):
        raise ValueError("Checkpoint replay does not match the established baseline")
    for index, values in enumerate(captured):
        values.astype("<f4").tofile(root / f"pc_reference/pre_gelu_{index}.bin")
    report = {"checkpoint": str(checkpoint), "checkpoint_sha256": expected,
              "config": config, "source_onnx_sha256": digest(MODEL_ROOT / "outputs/motor_stable23_v1_onnx/rul_fp32.onnx"),
              "replay_archive_sha256": digest(args.replay_archive),
              "state_entries": len(state), "state_floats": offset,
              "first_reference": float(references[0]), "checkpoint_first_prediction": first,
              "first_gelu_input_shapes": [list(x.shape) for x in captured],
              "native_full_model_verified": False, "f3p_verified": False, "npu_requested": False,
              "gelu": "Original exact-erf form, evaluated by host C++ std::erf between six native network segments",
              "provenance": {p.relative_to(root).as_posix(): digest(p) for p in root.rglob("*") if p.is_file()}}
    (root / "preparation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    with tarfile.open(archive, "w:gz", format=tarfile.GNU_FORMAT) as tf:
        def stable(member):
            member.mtime = 0
            return member
        tf.add(root, arcname=root.name, filter=stable)
    if digest(checkpoint) != expected:
        raise RuntimeError("Checkpoint changed during packaging")
    print(json.dumps({"root": str(root), "archive": str(archive), "size": archive.stat().st_size,
                      "checkpoint_first_prediction": first, "state_entries": len(state)}, indent=2))


if __name__ == "__main__":
    main()
