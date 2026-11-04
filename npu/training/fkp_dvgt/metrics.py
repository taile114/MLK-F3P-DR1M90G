"""Regression and prediction-interval metrics."""
import numpy as np


def regression_metrics(y, prediction, lower=None, upper=None):
    y, prediction = np.asarray(y).reshape(-1), np.asarray(prediction).reshape(-1)
    if not len(y) or y.shape != prediction.shape or not np.isfinite([y, prediction]).all():
        raise ValueError("Targets and predictions must be finite, nonempty and equally shaped")
    error = prediction-y
    result = {"rmse": float(np.sqrt(np.mean(error**2))), "mae": float(np.mean(abs(error))), "n": len(y)}
    if (lower is None) != (upper is None):
        raise ValueError("Both interval bounds must be supplied")
    if lower is not None:
        lower, upper = np.asarray(lower).reshape(-1), np.asarray(upper).reshape(-1)
        if lower.shape != y.shape or upper.shape != y.shape or not np.isfinite([lower, upper]).all() or np.any(lower > upper):
            raise ValueError("Invalid interval bounds")
        width = float(np.mean(upper-lower))
        result.update(picp=float(np.mean((lower <= y) & (y <= upper))), mpiw=width,
                      nmpiw=width/float(np.ptp(y)) if np.ptp(y) > 0 else None)
    return result
