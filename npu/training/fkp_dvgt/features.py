"""Extraction of 23 time-domain and frequency-domain signal features."""
import numpy as np
from scipy.fft import fft

def calculate_features(segment):
    num_sensors = segment.shape[1]
    feature_matrix = np.zeros((num_sensors, 23))
    for col in range(num_sensors):
        col_data = segment[:, col]
        N = len(col_data)
        f1 = np.mean(col_data)
        f2 = np.sqrt(np.sum((col_data - f1) ** 2) / (N - 1))
        f3 = (np.sum(np.sqrt(np.abs(col_data))) / N) ** 2
        f4 = np.sqrt(np.sum(col_data ** 2) / N)
        f5 = np.max(np.abs(col_data))
        f6 = np.sum((col_data - f1) ** 3) / ((N - 1) * f2 ** 3)
        f7 = np.sum((col_data - f1) ** 4) / ((N - 1) * f2 ** 4)
        f8 = f5 / f4
        f9 = f5 / f3
        f10 = f4 / np.mean(np.abs(col_data))
        f11 = f5 * f10 / f4
        fourier_transform = fft(col_data)
        s = np.abs(fourier_transform) ** 2
        K = len(s)
        f12 = np.mean(s)
        f13 = np.sum((s - f12) ** 2) / (K - 1)
        f14 = np.sum((s - f12) ** 3 / (K * np.sqrt(f13) ** 3))
        f15 = np.sum((s - f12) ** 4) / (K * f12 ** 3)
        f_k = np.fft.fftfreq(N)
        f16 = np.sum(f_k * s) / np.sum(s)
        f17 = np.sqrt(np.sum((f_k - f16) ** 2 * s) / K)
        f18 = np.sqrt(np.sum(f_k ** 2 * s) / np.sum(s))
        f19 = np.sqrt(np.sum(f_k ** 4 * s) / np.sum(f_k ** 2 * s))
        f20 = np.sum(f_k ** 2 * s) / np.sqrt(np.sum(f_k ** 4 * s) * np.sum(s))
        f21 = f17 / f16
        f22 = np.sum((f_k - f16) ** 3 * s) / (K * f17 ** 3)
        f23 = np.sum((f_k - f16) ** 4 * s) / (K * f17 ** 4)
        feature_matrix[col, :] = [f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23]
    feature_matrix = np.nan_to_num(feature_matrix, nan=0.0, posinf=1000000.0, neginf=-1000000.0)
    return feature_matrix
