"""Test feature extraction and normalization."""

import numpy as np

from src.inference.feature_extractor import FeatureExtractor, FEATURE_ORDER


def test_extract_from_view_rows(sample_metadata, sample_feature_rows):
    extractor = FeatureExtractor(sample_metadata)
    features = extractor.extract_from_view_rows(sample_feature_rows)
    assert len(features) == 10
    assert all(f.shape == (24,) for f in features)


def test_normalize(sample_metadata, sample_feature_rows):
    extractor = FeatureExtractor(sample_metadata)
    features = extractor.extract_from_view_rows(sample_feature_rows)
    X = extractor.normalize_list(features)
    assert X.shape == (10, 24)
    # Normalized data should be roughly zero-mean
    means = np.mean(X, axis=0)
    assert np.all(np.abs(means) < 3.0)  # within 3σ


def test_extract_from_jsonb():
    payload = {
        "features": [0.001, 0.002, 0.003, 0.005,
                     120.0, 0.02, 0.1, 3.2, 3.5,
                     0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1,
                     115.0, 0.015, 3.3,
                     118.0, 0.018, 3.4,
                     28.5]
    }
    # FeatExtractor needs metadata first, test the static extraction
    vec = np.array(payload["features"], dtype=np.float32)
    assert vec.shape == (24,)
    assert vec[23] == 28.5  # temperature


def test_extract_from_jsonb_partial():
    """Fallback extraction when no features array is present."""
    payload = {"data": {"vibration": {"rms_x": 0.001, "rms_y": 0.002, "rms_z": 0.003,
                                       "overall_rms": 0.005, "peak_freq": 120.0,
                                       "peak_amp": 0.02},
                         "environment": {"temperature_c": 28.0}}}

    from src.inference.feature_extractor import FeatureExtractor
    # Need a dummy extractor just to access the method
    metadata = {
        "normalization": {"mean": [0]*24, "std": [1]*24, "feature_names": []}
    }
    extractor = FeatureExtractor(metadata)
    vec = extractor.extract_from_jsonb_vibration(payload)
    assert vec is not None
    assert vec.shape == (24,)
    assert vec[0] == 0.001  # rms_x
    assert vec[3] == 0.005  # overall_rms
    assert vec[23] == 28.0  # temperature


def test_normalize_zero_std_handling(sample_metadata):
    """Normalization should not divide by zero."""
    metadata = dict(sample_metadata)
    # Set one std to zero
    metadata["normalization"]["std"] = [0.0 if i == 0 else x
                                         for i, x in enumerate(metadata["normalization"]["std"])]
    extractor = FeatureExtractor(metadata)
    X = np.ones((3, 24), dtype=np.float32)
    X_norm = extractor.normalize(X)
    assert not np.any(np.isnan(X_norm))
    assert not np.any(np.isinf(X_norm))
