"""Verify generated data using the existing, unmodified FKP-DVGT training code."""
import argparse
import copy
import csv
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np
import torch


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def main(args):
    sys.path.insert(0, str(args.model_root))
    from fkp_dvgt.config import DEFAULT_CONFIG
    from fkp_dvgt.data import load_dataset
    from fkp_dvgt.engine import build_model, train, evaluate, load_checkpoint, seed_everything
    source_hashes = {str(p.relative_to(args.model_root)): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (args.model_root / "fkp_dvgt").glob("*.py")}
    args.output.mkdir(parents=True, exist_ok=False)
    torch.set_num_threads(2)
    seed_everything(44)
    start = time.perf_counter()
    datasets, split_checks, identity = {}, {}, {}
    with np.load(args.data / "normalization.npz", allow_pickle=False) as normalizer:
        minimum, scale = normalizer["minimum"], normalizer["scale"]
        constant = normalizer["constant_features"]
    index = list(csv.DictReader((args.data / "sample_index.csv").open(encoding="utf-8-sig", newline="")))
    for split in ("train", "val", "test"):
        dataset = load_dataset(args.data / f"{split}.npz")
        datasets[split] = dataset
        x, y = dataset.tensors
        assert x.dtype == y.dtype == torch.float32
        assert tuple(x.shape[1:]) == (23, 110) and tuple(y.shape) == (len(x), 1)
        assert x.isfinite().all() and y.isfinite().all() and (y >= 0).all()
        with np.load(args.data / f"{split}.npz", allow_pickle=False) as stored:
            motors = set(stored["motor_ids"].tolist())
            identity[split] = motors
            records = [r for r in index if r["split"] == split]
            assert len(records) == len(x)
            for i in sorted({0, len(x) // 2, len(x) - 1}):
                record = records[i]
                blocks = json.loads(record["input_block_ids"])
                motor = int(record["motor_id"])
                with np.load(args.data / "features" / f"Motor{motor}.npz", allow_pickle=False) as f:
                    pos = {int(block): j for j, block in enumerate(f["block_ids"])}
                    raw = f["features"][[pos[b] for b in blocks]]
                    expected = (raw - minimum) / scale
                    expected[:, constant] = 0
                    expected = expected.astype(np.float32).reshape(110, 23).T
                    np.testing.assert_array_equal(x[i].numpy(), expected)
                    assert float(y[i]) == float(f["rul_cycles"][pos[blocks[-1]]])
        split_checks[split] = {"samples": len(dataset), "input_shape": list(x.shape),
                               "label_shape": list(y.shape), "finite": True,
                               "motor_ids": sorted(motors), "traceability_spot_checks": "pass"}
    assert all(not identity[a].intersection(identity[b]) for a, b in (("train", "val"), ("train", "test"), ("val", "test")))
    # Verify board replay bytes reproduce precisely the same tensors.
    board = np.fromfile(args.data / "board_replay" / "test_input_f32.bin", dtype="<f4").reshape(-1, 23, 110)
    np.testing.assert_array_equal(board, datasets["test"].tensors[0].numpy())
    model = build_model(DEFAULT_CONFIG["model"]).cpu()
    ids = np.linspace(0, len(datasets["train"]) - 1, 8, dtype=int)
    x, y = [t[ids] for t in datasets["train"].tensors]
    model.train()
    optimizer = torch.optim.Adam(model.parameters(), lr=DEFAULT_CONFIG["training"]["learning_rate"])
    before = model.output_layer[-1].base_weight.detach().clone()
    output = model(x)
    assert output.shape == y.shape and output.isfinite().all()
    loss = torch.nn.functional.mse_loss(output, y)
    loss.backward()
    with_grad = [p for p in model.parameters() if p.grad is not None]
    assert with_grad and all(p.grad.isfinite().all() for p in with_grad)
    unused = [name for name, p in model.named_parameters() if p.grad is None]
    optimizer.step()
    assert not torch.equal(before, model.output_layer[-1].base_weight.detach())
    one_step = {"status": "pass", "batch_size": len(x), "loss": float(loss.detach()),
                "finite_gradients": True, "parameter_updated": True,
                "unused_parameters_in_original_forward": unused}
    print(json.dumps({"one_step": one_step}), flush=True)
    config = copy.deepcopy(DEFAULT_CONFIG)
    config["training"].update(epochs=3, batch_size=32)
    checkpoint = train(config, args.data / "train.npz", args.data / "val.npz",
                       args.output / "short_train", device="cpu", variant="full")
    history = [json.loads(line) for line in (args.output / "short_train" / "history.jsonl").read_text().splitlines()]
    assert len(history) == 3 and all(np.isfinite([r["train_mse"], r["val_rmse"]]).all() for r in history)
    restored, saved_config = load_checkpoint(checkpoint, "cpu")
    restored.eval()
    predictions, metrics = evaluate(restored, datasets["test"], "cpu", 32)
    assert np.isfinite(predictions["mean"]).all()
    again, _ = load_checkpoint(checkpoint, "cpu")
    again.eval()
    with torch.no_grad():
        np.testing.assert_array_equal(restored(x).numpy(), again(x).numpy())
    print(json.dumps({"short_training": "pass", "test_metrics_not_final_accuracy": metrics}), flush=True)
    # Small-batch capacity check uses varied labels and disables dropout only for
    # this diagnostic. These fitted weights are not a deployable trained model.
    seed_everything(44)
    tiny = build_model(DEFAULT_CONFIG["model"]).cpu()
    tiny.eval()
    x_all, y_all = datasets["train"].tensors
    order = np.argsort(y_all.numpy().ravel(), kind="stable")
    ids = order[np.linspace(0, len(order) - 1, 8, dtype=int)]
    tiny_x, tiny_y = x_all[ids], y_all[ids]
    tiny_optimizer = torch.optim.Adam(tiny.parameters(), lr=0.002)
    with torch.no_grad():
        initial = float(torch.nn.functional.mse_loss(tiny(tiny_x), tiny_y))
    curve = []
    for step in range(1, 301):
        tiny_optimizer.zero_grad(set_to_none=True)
        tiny_loss = torch.nn.functional.mse_loss(tiny(tiny_x), tiny_y)
        assert torch.isfinite(tiny_loss)
        tiny_loss.backward()
        assert all(p.grad.isfinite().all() for p in tiny.parameters() if p.grad is not None)
        tiny_optimizer.step()
        if step % 25 == 0:
            with torch.no_grad():
                current = float(torch.nn.functional.mse_loss(tiny(tiny_x), tiny_y))
            curve.append({"step": step, "mse": current})
            print(json.dumps({"capacity_check_step": step, "mse": current, "initial_mse": initial}), flush=True)
            if current < initial * 0.01:
                break
    with torch.no_grad():
        final = float(torch.nn.functional.mse_loss(tiny(tiny_x), tiny_y))
    capacity = {"status": "pass" if final < initial * 0.1 else "insufficient_loss_reduction",
                "initial_mse": initial, "final_mse": final, "ratio": final / initial,
                "samples": len(ids), "labels": tiny_y.ravel().tolist(), "steps": step,
                "dropout_disabled_for_this_diagnostic": True, "curve": curve}
    after_hashes = {str(p.relative_to(args.model_root)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in (args.model_root / "fkp_dvgt").glob("*.py")}
    assert source_hashes == after_hashes
    result = {"status": "pass" if capacity["status"] == "pass" else "data_and_training_pass_capacity_check_inconclusive",
              "python": sys.version, "torch": str(torch.__version__), "numpy": np.__version__,
              "device": "cpu", "split_checks": split_checks, "motor_separation": "pass",
              "board_replay_byte_comparison": "pass", "one_step": one_step,
              "short_training_epochs": 3, "short_training_history": history,
              "checkpoint_reload_and_prediction": "pass", "test_metrics_not_final_accuracy": metrics,
              "small_batch_capacity": capacity, "model_source_unchanged": True,
              "source_sha256": source_hashes, "elapsed_seconds": time.perf_counter() - start,
              "purpose": "data compatibility and trainability smoke check; not a final model evaluation"}
    write_json(args.output / "training_validation.json", result)
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-root", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    main(parser.parse_args())
