"""Default model, training, and uncertainty configuration."""

DEFAULT_CONFIG = {
    "model": {
        "num_nodes": 11,
        "time_length": 10,
        "d_model": [21, 21],
        "num_heads": 3,
        "lambda_param": 0.5,
        "d_ff": [140, 140],
        "dropout": 0.15,
        "num_blocks": 2,
    },
    "training": {
        "epochs": 150,
        "batch_size": 100,
        "learning_rate": 0.005,
        "weight_decay": 0.0001,
        "seed": 44,
        "shuffle": False,
    },
    "uncertainty": {
        "draws": 60,
        "graph_probability": 0.9,
        "noise_std": 0.1,
        "z": 3.0,
        "mc_dropout": True,
    },
}
