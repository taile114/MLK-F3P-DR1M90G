"""Plots for RUL predictions, intervals, and feature sensitivity."""
from pathlib import Path
import numpy as np


def plot_predictions(input_path, output_path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    with np.load(input_path, allow_pickle=False) as data:
        fig, ax = plt.subplots(figsize=(9, 4))
        axis = np.arange(len(data["target"]))
        ax.plot(axis, data["target"], color="black", label="Target")
        ax.plot(axis, data["mean"], label="Prediction")
        if "lower" in data:
            ax.fill_between(axis, data["lower"], data["upper"], alpha=0.25, label="Predictive interval")
        ax.set(xlabel="Sample index", ylabel="RUL")
        ax.legend()
        fig.tight_layout()
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(output_path, dpi=180)
        plt.close(fig)


def plot_sensitivity(input_path, output_path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    with np.load(input_path, allow_pickle=False) as data:
        values = data["importance"]
    if values.shape != (10, 11, 23):
        raise ValueError("Expected sensitivity array [10,11,23]")
    fig, axes = plt.subplots(1, 2, figsize=(12, 4), gridspec_kw={"width_ratios": [2, 1]})
    image = axes[0].imshow(values.mean(0), aspect="auto", origin="lower")
    axes[0].set(xlabel="Feature index", ylabel="Sensor index", title="Mean sensitivity")
    fig.colorbar(image, ax=axes[0])
    axes[1].bar(np.arange(11), values.sum(-1).mean(0))
    axes[1].set(xlabel="Sensor index", ylabel="Summed sensitivity")
    fig.tight_layout()
    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=180)
    plt.close(fig)
