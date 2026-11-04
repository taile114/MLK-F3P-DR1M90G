"""Explicit tensor layouts and safe local array loading."""
from pathlib import Path
import numpy as np
import torch
from torch.utils.data import TensorDataset


def canonical_samples(samples):
    x = torch.as_tensor(samples, dtype=torch.float32)
    if x.ndim == 4 and tuple(x.shape[1:]) == (10, 11, 23):
        x = x.reshape(len(x), 110, 23).transpose(1, 2)
    elif x.ndim == 3 and tuple(x.shape[1:]) == (110, 23):
        x = x.transpose(1, 2)
    elif x.ndim != 3 or tuple(x.shape[1:]) != (23, 110):
        raise ValueError("samples must have shape [N,10,11,23], [N,110,23], or [N,23,110]")
    if len(x) == 0 or not torch.isfinite(x).all():
        raise ValueError("samples must be nonempty and finite")
    return x.contiguous()


def load_dataset(path):
    path = Path(path)
    if path.suffix == ".npz":
        with np.load(path, allow_pickle=False) as data:
            x, y = canonical_samples(data["samples"]), torch.tensor(data["labels"], dtype=torch.float32)
    elif path.suffix == ".pt":
        data = torch.load(path, map_location="cpu", weights_only=True)
        x, y = canonical_samples(data["samples"]), torch.as_tensor(data["labels"], dtype=torch.float32)
    else:
        raise ValueError("Use an .npz or tensor-only .pt file; see README.md")
    if y.numel() != len(x) or not torch.isfinite(y).all():
        raise ValueError("One finite scalar label is required per sample")
    return TensorDataset(x, y.reshape(-1, 1))
