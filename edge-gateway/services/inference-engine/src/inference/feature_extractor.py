"""Extract and normalize 24-dim feature vectors from DB rows or JSONB payloads."""

import json
from typing import Optional

import numpy as np
import structlog

logger = structlog.get_logger(__name__)

FEATURE_ORDER = [
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


class FeatureExtractor:
    def __init__(self, metadata: dict):
        norm = metadata["normalization"]
        self.norm_mean = np.array(norm["mean"], dtype=np.float32)
        self.norm_std = np.array(norm["std"], dtype=np.float32)
        self.clip = 1e-10
        logger.info("feature extractor ready", dim=len(self.norm_mean))

    @classmethod
    def from_metadata_file(cls, path: str) -> "FeatureExtractor":
        with open(path) as f:
            metadata = json.load(f)
        return cls(metadata)

    def extract_from_view_rows(self, rows: list) -> list:
        """Extract feature vectors from feature_vector_view rows (from DB)."""
        features_list = []
        for row in rows:
            vec = np.array([row.get(col, 0.0) or 0.0 for col in FEATURE_ORDER],
                           dtype=np.float32)
            features_list.append(vec)
        return features_list

    def extract_from_jsonb(self, payload: dict) -> Optional[np.ndarray]:
        """Extract a single feature vector from a raw JSONB MQTT payload."""
        features = payload.get("features")
        if features and len(features) == 24:
            return np.array(features, dtype=np.float32)
        return None

    def extract_from_jsonb_vibration(self, payload: dict) -> Optional[np.ndarray]:
        """Fallback: extract partial features from vibration JSONB (6-dim subset).
        Used when 24-dim features are unavailable (pre-firmware-update data)."""
        vib = payload.get("data", {}).get("vibration", {})
        env = payload.get("data", {}).get("environment", {})
        if not vib:
            return None
        temp = env.get("temperature_c", 25.0)
        vec = np.array([
            vib.get("rms_x", 0.0),
            vib.get("rms_y", 0.0),
            vib.get("rms_z", 0.0),
            vib.get("overall_rms", 0.0),
            vib.get("peak_freq", 0.0),
            vib.get("peak_amp", 0.0),
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,  # skew/kurt/crest/bands (unavailable)
            0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0,
            0.0, 0.0, 0.0,
            temp,
        ], dtype=np.float32)
        return vec

    def normalize(self, X: np.ndarray) -> np.ndarray:
        """Apply z-score normalization: (X - mean) / std."""
        return (X - self.norm_mean) / np.maximum(self.norm_std, self.clip)

    def normalize_list(self, features_list: list) -> np.ndarray:
        if not features_list:
            return np.empty((0, 24), dtype=np.float32)
        X = np.stack(features_list, axis=0).astype(np.float32)
        return self.normalize(X)
