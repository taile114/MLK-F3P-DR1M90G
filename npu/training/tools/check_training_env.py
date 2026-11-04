"""Check the project CUDA environment with the existing model and real inputs."""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.metadata
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import numpy as np
import torch


def command_output(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        return result.stdout.strip() if result.returncode == 0 else result.stderr.strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        return str(error)


def main(args):
    import scipy
    import sklearn
    import pandas
    import matplotlib
    matplotlib.use("Agg")
    from fkp_dvgt.config import DEFAULT_CONFIG
    from fkp_dvgt.data import load_dataset
    from fkp_dvgt.engine import build_model, train, evaluate, load_checkpoint, seed_everything
    from fkp_dvgt.uncertainty import predict_mc
    from fkp_dvgt.plotting import plot_predictions

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable; use the project .venv and its cu126 torch wheel")
    if args.output.exists():
        raise FileExistsError(f"Use a new check output directory: {args.output}")
    args.output.mkdir(parents=True)
    torch.set_num_threads(2)
    seed_everything(44)
    device = torch.device("cuda:0")
    source_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (ROOT / "fkp_dvgt").glob("*.py")}
    versions = {name: importlib.metadata.version(name) for name in
                ("torch", "numpy", "scipy", "scikit-learn", "matplotlib", "pandas")}
    print(json.dumps({"python": sys.version, "versions": versions,
                      "gpu": torch.cuda.get_device_name(0), "cuda": torch.version.cuda},
                     ensure_ascii=False), flush=True)
    loaded = {split: load_dataset(args.data / f"{split}.npz") for split in ("train", "val", "test")}
    batch = min(args.batch_size, len(loaded["train"]))
    x, y = [t[:batch].to(device) for t in loaded["train"].tensors]
    model = build_model(DEFAULT_CONFIG["model"]).to(device)
    model.train()
    optimizer = torch.optim.Adam(model.parameters(), lr=DEFAULT_CONFIG["training"]["learning_rate"])
    torch.cuda.reset_peak_memory_stats()
    torch.cuda.synchronize()
    started = time.perf_counter()
    output = model(x)
    assert output.shape == y.shape and output.isfinite().all()
    loss = torch.nn.functional.mse_loss(output, y)
    loss.backward()
    assert torch.isfinite(loss)
    assert all(p.grad.isfinite().all() for p in model.parameters() if p.grad is not None)
    optimizer.step()
    torch.cuda.synchronize()
    batch_seconds = time.perf_counter() - started
    peak_mib = torch.cuda.max_memory_allocated() / 1024 ** 2
    print(json.dumps({"cuda_forward_backward": "pass", "batch_size": batch,
                      "loss": float(loss.detach()), "seconds": batch_seconds,
                      "peak_allocated_mib": peak_mib}), flush=True)
    del model, optimizer, output, loss, x, y
    torch.cuda.empty_cache()
    config = copy.deepcopy(DEFAULT_CONFIG)
    config["training"].update(epochs=args.epochs, batch_size=args.batch_size)
    checkpoint = train(config, args.data / "train.npz", args.data / "val.npz",
                       args.output / "cuda_smoke", device="cuda", variant="full")
    restored, _ = load_checkpoint(checkpoint, "cuda")
    predictions, metrics = evaluate(restored, loaded["val"], "cuda", args.batch_size)
    assert np.isfinite(predictions["mean"]).all()
    np.savez_compressed(args.output / "validation_predictions.npz", **predictions)
    plot_predictions(args.output / "validation_predictions.npz", args.output / "validation_predictions.png")
    uq = predict_mc(restored, loaded["val"].tensors[0][:1].to(device), draws=2)
    assert all(t.isfinite().all() for t in uq.values())
    # A saved GPU checkpoint must also load on the CPU for deployment tooling.
    cpu_model, _ = load_checkpoint(checkpoint, "cpu")
    cpu_model.eval()
    with torch.no_grad():
        assert cpu_model(loaded["val"].tensors[0][:1]).isfinite().all()
    assert source_hashes == {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in (ROOT / "fkp_dvgt").glob("*.py")}
    report = {"status": "pass", "python": sys.version, "executable": sys.executable,
              "versions": versions, "cuda_available": True, "torch_cuda_runtime": torch.version.cuda,
              "cudnn_version": torch.backends.cudnn.version(), "gpu": torch.cuda.get_device_name(0),
              "gpu_capability": list(torch.cuda.get_device_capability(0)),
              "gpu_memory_total_mib": torch.cuda.get_device_properties(0).total_memory / 1024 ** 2,
              "driver": command_output(["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"]),
              "system_cuda_toolkit": command_output(["nvcc", "--version"]),
              "batch_size": batch, "batch_forward_backward_seconds": batch_seconds,
              "batch_peak_allocated_mib": peak_mib, "short_training_epochs": args.epochs,
              "samples": {s: len(d) for s, d in loaded.items()},
              "gpu_checkpoint_reload": "pass", "gpu_checkpoint_cpu_reload": "pass",
              "uq_two_draws_import_and_execution_check": "pass", "model_source_unchanged": True,
              "matplotlib_plot_export": "pass",
              "validation_metrics_not_final_accuracy": metrics,
              "purpose": "environment verification; not a final training run"}
    (args.output / "environment.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                                 encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=ROOT / "processed/run_to_failure_5motors_v1")
    parser.add_argument("--output", type=Path, default=ROOT / "outputs/environment_check")
    parser.add_argument("--batch-size", type=int, default=100)
    parser.add_argument("--epochs", type=int, default=2)
    main(parser.parse_args())
