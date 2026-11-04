"""Monte Carlo predictive uncertainty estimation."""
import torch
from torch import nn


@torch.no_grad()
def predict_mc(model, x, draws=60, graph_probability=0.9, noise_std=0.1, z=3.0, mc_dropout=True):
    if draws < 2 or noise_std < 0 or z <= 0 or not 0 <= graph_probability <= 1:
        raise ValueError("Require draws >= 2, noise_std >= 0, z > 0 and graph_probability in [0,1]")
    states = {module: module.training for module in model.modules()}
    try:
        model.eval()
        if mc_dropout:
            for module in model.modules():
                if isinstance(module, nn.Dropout):
                    module.train()
        predictions = torch.stack([
            model(x + noise_std * torch.randn_like(x), graph_probability=graph_probability).flatten()
            for _ in range(draws)
        ])
        mean, std = predictions.mean(0), predictions.std(0, unbiased=True)
        return {"mean": mean, "std": std, "lower": mean-z*std, "upper": mean+z*std}
    finally:
        for module, state in states.items():
            module.training = state
