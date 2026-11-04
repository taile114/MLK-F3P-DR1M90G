"""Model training, checkpointing, and inference."""
import json
import random
from pathlib import Path
import numpy as np
import torch
from torch import nn
from torch.utils.data import DataLoader
from .data import load_dataset
from .model import DVGTformer_model, KANLayer
from .metrics import regression_metrics
from .uncertainty import predict_mc


def seed_everything(seed):
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = True


def build_model(config, variant="full"):
    model = DVGTformer_model(**config)
    if variant == "without-kan":
        def replace(module):
            for name, child in list(module.named_children()):
                if isinstance(child, KANLayer):
                    setattr(module, name, nn.Linear(child.in_features, child.out_features))
                else:
                    replace(child)
        replace(model)
    elif variant != "full":
        raise ValueError("Unknown architecture variant")
    return model


def save_json(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2, allow_nan=False)+"\n", encoding="utf-8")


@torch.no_grad()
def evaluate(model, dataset, device, batch_size, uncertainty=None):
    model.eval()
    collected = {"target": [], "mean": []}
    if uncertainty is not None:
        collected.update(std=[], lower=[], upper=[])
    for x, y in DataLoader(dataset, batch_size=batch_size, shuffle=False):
        x = x.to(device)
        prediction = predict_mc(model, x, **uncertainty) if uncertainty is not None else {"mean": model(x).flatten()}
        collected["target"].append(y.flatten().numpy())
        for key, value in prediction.items():
            collected[key].append(value.cpu().numpy())
    arrays = {k: np.concatenate(v) for k, v in collected.items()}
    metrics = regression_metrics(arrays["target"], arrays["mean"], arrays.get("lower"), arrays.get("upper"))
    return arrays, metrics


def train(config, train_path, val_path, output, device="cpu", variant="full"):
    if Path(train_path).resolve() == Path(val_path).resolve():
        raise ValueError("Training and validation paths must differ")
    params = config["training"]
    if params["epochs"] < 1 or params["batch_size"] < 1:
        raise ValueError("epochs and batch_size must be positive")
    seed_everything(params["seed"])
    train_data, val_data = load_dataset(train_path), load_dataset(val_path)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    save_json(output/"config.json", config)
    model = build_model(config["model"], variant).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=params["learning_rate"], weight_decay=params["weight_decay"])
    loader = DataLoader(
        train_data,
        batch_size=params["batch_size"],
        shuffle=params.get("shuffle", False),
        drop_last=False,
    )
    best = float("inf")
    for epoch in range(1, params["epochs"]+1):
        model.train()
        total = 0.0
        for x, y in loader:
            optimizer.zero_grad(set_to_none=True)
            loss = nn.functional.mse_loss(model(x.to(device)), y.to(device))
            if not torch.isfinite(loss):
                raise RuntimeError("Nonfinite loss; inspect feature scales and learning rate")
            loss.backward()
            optimizer.step()
            total += loss.item()*len(x)
        _, metrics = evaluate(model, val_data, device, params["batch_size"])
        row = {"epoch": epoch, "train_mse": total/len(train_data), "val_rmse": metrics["rmse"]}
        with (output/"history.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps(row)+"\n")
        print(json.dumps(row), flush=True)
        if metrics["rmse"] < best:
            best = metrics["rmse"]
            torch.save({"model_state_dict": model.state_dict(), "config": config, "variant": variant,
                        "epoch": epoch, "validation_rmse": best}, output/"best.pt")
    save_json(output/"run.json", {"seed": params["seed"], "device": str(device), "torch": str(torch.__version__),
                                   "numpy": np.__version__, "variant": variant, "selection": "validation_rmse"})
    return output/"best.pt"


def load_checkpoint(path, device="cpu"):
    checkpoint = torch.load(path, map_location=device, weights_only=True)
    if "config" not in checkpoint:
        raise ValueError("Checkpoint does not contain the model configuration")
    model = build_model(checkpoint["config"]["model"], checkpoint.get("variant", "full")).to(device)
    model.load_state_dict(checkpoint["model_state_dict"], strict=True)
    return model, checkpoint["config"]
