"""Cluster-representative adjacency replication for stochastic inference."""
import torch
from sklearn.cluster import AgglomerativeClustering


def _sample(features, adjacency, probability):
    labels = AgglomerativeClustering(n_clusters=min(3, len(features))).fit_predict(
        features.detach().cpu().numpy()
    )
    # A single index map avoids order-dependent in-place row/column overwrites.
    mapping = torch.arange(len(features), device=features.device)
    for label in set(labels.tolist()):
        ids = torch.as_tensor((labels == label).nonzero()[0], device=features.device)
        core = ids[torch.randint(len(ids), (), device=features.device)]
        chosen = torch.rand(len(ids), device=features.device) < probability
        mapping[ids[chosen]] = core
    return adjacency[mapping[:, None], mapping[None, :]]


def sample_graphs(x, temporal, spatial, probability):
    if not 0 <= probability <= 1:
        raise ValueError("graph probability must lie in [0, 1]")
    if probability == 0:
        return temporal, spatial
    return (
        torch.stack([_sample(v, a, probability) for v, a in zip(x, temporal)]),
        torch.stack([_sample(v.T, a, probability) for v, a in zip(x, spatial)]),
    )
