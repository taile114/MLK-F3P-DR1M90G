"""Portable command-line entry points; all I/O paths are explicit."""
import argparse
import copy
import json
from pathlib import Path
import numpy as np
import torch
from .data import load_dataset
from .config import DEFAULT_CONFIG
from .engine import train, evaluate, load_checkpoint, save_json, seed_everything


def main():
    parser = argparse.ArgumentParser(description="FKP-DVGT research pipeline")
    subs = parser.add_subparsers(dest="command", required=True)
    p = subs.add_parser("prepare", help="Build motor-separated splits from aligned raw .npy arrays")
    p.add_argument("--manifest", required=True)
    p.add_argument("--output", required=True)
    p = subs.add_parser("train")
    p.add_argument("--train", required=True)
    p.add_argument("--val", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--device", default="cpu")
    p.add_argument("--variant", choices=["full", "without-kan"], default="full")
    p.add_argument("--epochs", type=int)
    p.add_argument("--batch-size", type=int)
    p.add_argument("--learning-rate", type=float)
    for name in ("evaluate", "explain"):
        p = subs.add_parser(name)
        p.add_argument("--checkpoint", required=True)
        p.add_argument("--data", required=True)
        p.add_argument("--output", required=True)
        p.add_argument("--device", default="cpu")
        p.add_argument("--seed", type=int, default=44)
        if name == "evaluate":
            p.add_argument("--batch-size", type=int, default=16)
            p.add_argument("--uq", action="store_true")
            p.add_argument("--draws", type=int)
            p.add_argument("--graph-probability", type=float)
            p.add_argument("--noise-std", type=float)
            p.add_argument("--z", type=float)
            p.add_argument("--no-mc-dropout", action="store_true")
        else:
            p.add_argument("--index", type=int, default=0)
            p.add_argument("--smooth-samples", type=int, default=8)
            p.add_argument("--noise-std", type=float, default=0.01)
            p.add_argument("--gradient-only", action="store_true")
    p = subs.add_parser("plot", help="Plot RUL predictions and intervals")
    p.add_argument("--input", required=True)
    p.add_argument("--output", required=True)
    p = subs.add_parser("plot-sensitivity", help="Plot saved feature sensitivity")
    p.add_argument("--input", required=True)
    p.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.command == "prepare":
        from .preprocess import prepare
        prepare(args.manifest, args.output)
    elif args.command == "train":
        config = copy.deepcopy(DEFAULT_CONFIG)
        if args.epochs is not None:
            config["training"]["epochs"] = args.epochs
        if args.batch_size is not None:
            config["training"]["batch_size"] = args.batch_size
        if args.learning_rate is not None:
            config["training"]["learning_rate"] = args.learning_rate
        train(config, args.train, args.val, args.output, args.device, args.variant)
    elif args.command == "plot":
        from .plotting import plot_predictions
        plot_predictions(args.input, args.output)
    elif args.command == "plot-sensitivity":
        from .plotting import plot_sensitivity
        plot_sensitivity(args.input, args.output)
    else:
        seed_everything(args.seed)
        model, config = load_checkpoint(args.checkpoint, args.device)
        dataset = load_dataset(args.data)
        output = Path(args.output)
        output.mkdir(parents=True, exist_ok=False)
        if args.command == "evaluate":
            uq = dict(config["uncertainty"]) if args.uq else None
            if not args.uq and (any(getattr(args, k) is not None for k in ("draws", "graph_probability", "noise_std", "z")) or args.no_mc_dropout):
                parser.error("Uncertainty overrides require --uq")
            if uq is not None:
                for k in ("draws", "graph_probability", "noise_std", "z"):
                    if getattr(args, k) is not None:
                        uq[k] = getattr(args, k)
                if args.no_mc_dropout:
                    uq["mc_dropout"] = False
            arrays, metrics = evaluate(model, dataset, args.device, args.batch_size, uq)
            np.savez_compressed(output/"predictions.npz", **arrays)
            save_json(output/"metrics.json", metrics)
            save_json(output/"inference.json", {"seed": args.seed, "uncertainty": uq, "batch_size": args.batch_size})
            print(json.dumps(metrics))
        else:
            from .interpret import sensitivity
            if not 0 <= args.index < len(dataset):
                raise ValueError("index outside dataset")
            importance = sensitivity(model, dataset[args.index][0].unsqueeze(0).to(args.device),
                                     args.smooth_samples, args.noise_std, not args.gradient_only)[0].cpu().numpy()
            np.savez_compressed(output/"sensitivity.npz", importance=importance,
                                sensor_importance=importance.sum(-1).mean(0))
            save_json(output/"explanation.json", {"index": args.index, "seed": args.seed,
                      "smooth_samples": args.smooth_samples, "noise_std": args.noise_std,
                      "method": "gradient" if args.gradient_only else "gradient_x_input"})
