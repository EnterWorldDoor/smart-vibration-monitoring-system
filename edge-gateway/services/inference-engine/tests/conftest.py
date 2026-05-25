"""Shared test fixtures for inference-engine tests."""

import json
import os
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

# Add services/inference-engine/ (parent of src/) to path for src.* imports
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


@pytest.fixture
def sample_metadata():
    return {
        "model_name": "vibration_autoencoder",
        "version": "1.0.0",
        "input_dim": 24,
        "anomaly_threshold": 0.15,
        "normalization": {
            "mean": [0.005] * 4 + [120.0, 0.02, 0.0, 3.0, 3.5] +
                    [0.12] * 8 + [120.0, 0.015, 3.5] + [120.0, 0.015, 3.5] + [30.0],
            "std": [0.02] * 4 + [80.0, 0.03, 1.5, 3.0, 2.5] +
                   [0.05] * 8 + [80.0, 0.02, 2.5] + [80.0, 0.02, 2.5] + [10.0],
            "feature_names": [
                "rms_x", "rms_y", "rms_z", "overall_rms",
                "peak_freq_x", "peak_amp_x", "skewness_x", "kurtosis_x", "crest_factor_x",
                "band_energy_x_0", "band_energy_x_1", "band_energy_x_2", "band_energy_x_3",
                "band_energy_x_4", "band_energy_x_5", "band_energy_x_6", "band_energy_x_7",
                "peak_freq_y", "peak_amp_y", "crest_factor_y",
                "peak_freq_z", "peak_amp_z", "crest_factor_z",
                "temperature_c",
            ],
        },
    }


@pytest.fixture
def metadata_file(sample_metadata, tmp_path):
    """Write sample metadata to a temp JSON file."""
    p = tmp_path / "autoencoder_metadata.json"
    with open(p, "w") as f:
        json.dump(sample_metadata, f)
    return str(p)


@pytest.fixture
def sample_feature_rows():
    """Generate 10 rows mimicking feature_vector_view output."""
    rows = []
    col_names = [
        "feat_rms_x", "feat_rms_y", "feat_rms_z", "feat_overall_rms",
        "feat_peak_freq_x", "feat_peak_amp_x", "feat_skewness_x",
        "feat_kurtosis_x", "feat_crest_factor_x",
        "feat_band_energy_x_0", "feat_band_energy_x_1",
        "feat_band_energy_x_2", "feat_band_energy_x_3",
        "feat_band_energy_x_4", "feat_band_energy_x_5",
        "feat_band_energy_x_6", "feat_band_energy_x_7",
        "feat_peak_freq_y", "feat_peak_amp_y", "feat_crest_factor_y",
        "feat_peak_freq_z", "feat_peak_amp_z", "feat_crest_factor_z",
        "feat_temperature_c",
    ]
    np.random.seed(42)
    for i in range(10):
        row = {"time": f"2026-05-25T10:00:{i:02d}Z"}
        for j, name in enumerate(col_names):
            # Normal-condition values
            if "rms" in name and "overall" not in name:
                row[name] = float(np.random.normal(0.005, 0.002))
            elif "overall_rms" in name:
                row[name] = float(np.random.normal(0.01, 0.003))
            elif "peak_freq" in name:
                row[name] = float(np.random.normal(120, 10))
            elif "peak_amp" in name:
                row[name] = float(np.random.normal(0.02, 0.003))
            elif "temperature" in name:
                row[name] = float(np.random.normal(30, 2))
            elif "skewness" in name:
                row[name] = float(np.random.normal(0, 0.5))
            elif "kurtosis" in name:
                row[name] = float(np.random.normal(3, 0.5))
            elif "crest_factor" in name:
                row[name] = float(np.random.normal(3.5, 0.3))
            elif "band_energy" in name:
                row[name] = float(np.random.normal(0.125, 0.02))
            else:
                row[name] = 0.0
        rows.append(row)
    return rows


@pytest.fixture
def sample_vibration_rows():
    """Generate vibration_view-like rows for trend analysis."""
    rows = []
    np.random.seed(42)
    for i in range(30):
        rows.append({
            "time": f"2026-05-25T10:00:{i:02d}Z",
            "rms_x": float(np.random.normal(0.005, 0.002)),
            "rms_y": float(np.random.normal(0.005, 0.002)),
            "rms_z": float(np.random.normal(0.005, 0.002)),
            "overall_rms": float(np.random.normal(0.01, 0.003) + i * 0.001),
            "peak_frequency_hz": float(np.random.normal(120, 5)),
            "peak_amplitude_g": float(np.random.normal(0.02, 0.003)),
        })
    return rows


@pytest.fixture
def sample_vibration_rows_rising():
    """Vibration data with a clear rising RMS trend (fault simulation)."""
    rows = []
    np.random.seed(99)
    for i in range(30):
        rows.append({
            "time": f"2026-05-25T10:00:{i:02d}Z",
            "rms_x": float(np.random.normal(0.005, 0.001)),
            "rms_y": float(np.random.normal(0.005, 0.001)),
            "rms_z": float(np.random.normal(0.005, 0.001)),
            "overall_rms": float(2.0 + i * 0.5 + np.random.normal(0, 0.3)),
            "peak_frequency_hz": float(120 + i * 2 + np.random.normal(0, 1)),
            "peak_amplitude_g": float(0.02 + i * 0.01 + np.random.normal(0, 0.005)),
        })
    return rows
