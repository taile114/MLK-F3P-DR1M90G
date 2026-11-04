"""Build splits from user-provided, aligned motor recordings."""
import json
from pathlib import Path
import numpy as np
from .features import calculate_features


def prepare(manifest_path, output):
    manifest_path, output = Path(manifest_path), Path(output)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    records = manifest["motors"]
    ids = [str(r["id"]) for r in records]
    if len(ids) != len(set(ids)):
        raise ValueError("Each motor must occur exactly once; split by motor, never overlapping windows")
    if {r["split"] for r in records} != {"train", "val", "test"}:
        raise ValueError("Specify train, val and test motors explicitly")
    paths = [(manifest_path.parent/r["path"]).resolve() for r in records]
    if len(set(paths)) != len(paths):
        raise ValueError("A recording cannot appear in multiple splits")
    segment, stride = manifest.get("segment_length", 10000), manifest.get("stride", 5000)
    if segment < 2 or stride < 1:
        raise ValueError("Require segment_length >= 2 and stride >= 1")
    features = []
    for record, path in zip(records, paths):
        raw = np.load(path, allow_pickle=False)
        if raw.ndim != 2 or raw.shape[1] != 11 or not np.isfinite(raw).all():
            raise ValueError(f"{record['id']}: expected finite [time,11] aligned sensor array")
        starts = range(0, len(raw)-segment+1, stride)
        if len(starts) < 10:
            raise ValueError(f"{record['id']}: need at least 10 feature segments")
        with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
            features.append(np.stack([calculate_features(raw[i:i+segment]) for i in starts]))
    training = np.concatenate([f for r, f in zip(records, features) if r["split"] == "train"])
    minimum, maximum = training.min(0), training.max(0)
    scale = np.where(maximum > minimum, maximum-minimum, 1.0)
    output.mkdir(parents=True, exist_ok=False)
    np.savez(output/"normalization.npz", minimum=minimum, scale=scale)
    metadata = {"segment_length": segment, "stride": stride, "window": 10,
                "label_policy": "linear graph sequence to zero; user-supplied initial RUL",
                "feature_formulas": "full_fft_23_features", "motors": []}
    for split in ("train", "val", "test"):
        xs, ys = [], []
        for record, f in zip(records, features):
            if record["split"] != split:
                continue
            initial = float(record["initial_rul"])
            if not np.isfinite(initial) or initial <= 0:
                raise ValueError("initial_rul must be finite and positive")
            f = (f-minimum)/scale
            windows = np.stack([f[i:i+10] for i in range(len(f)-9)])
            labels = np.linspace(initial, 0, len(windows)) if len(windows) > 1 else np.zeros(1)
            xs.append(windows.astype(np.float32))
            ys.append(labels.astype(np.float32))
            metadata["motors"].append({"id": str(record["id"]), "split": split,
                                       "samples": len(windows), "initial_rul": initial})
        np.savez_compressed(output/f"{split}.npz", samples=np.concatenate(xs), labels=np.concatenate(ys))
    (output/"preprocessing.json").write_text(json.dumps(metadata, indent=2)+"\n", encoding="utf-8")
