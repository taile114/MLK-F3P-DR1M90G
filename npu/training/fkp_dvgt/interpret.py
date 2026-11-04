"""Sensor-wise sensitivity analysis for KAN feature fusion."""
import torch


def sensitivity(model, x, smooth_samples=8, noise_std=0.01, times_input=True):
    if smooth_samples < 1 or noise_std < 0:
        raise ValueError("Invalid SmoothGrad settings")
    if x.ndim != 3 or tuple(x.shape[1:]) != (23, 110):
        raise ValueError("Expected input [batch, 23, 110]")
    states = {m: m.training for m in model.modules()}
    try:
        model.eval()
        # Apply the shared feature transform before evaluating each sensor's
        # 23-to-1 KAN fusion map.
        transformed = model.linear_x(x.transpose(1, 2)).reshape(-1, 10, 11, 23).detach()
        sensor_importance = []
        for sensor, layer in enumerate(model.linear_sample):
            base = transformed[:, :, sensor, :]
            gradients = []
            for _ in range(smooth_samples):
                noisy = (base + noise_std * torch.randn_like(base)).requires_grad_(True)
                gradient, = torch.autograd.grad(layer(noisy).sum(), noisy)
                gradients.append(gradient)
            average_gradient = torch.stack(gradients).mean(0)
            value = average_gradient * base if times_input else average_gradient
            sensor_importance.append(value.abs())
        return torch.stack(sensor_importance, dim=2).detach()
    finally:
        for module, state in states.items():
            module.training = state
