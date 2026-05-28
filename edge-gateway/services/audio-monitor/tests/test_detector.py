"""Test ThresholdDetector — sigma rules, sustain escalation, learning period."""

import numpy as np
import pytest

from src.anomaly.detector import ThresholdDetector, AnomalyResult
from src.audio.processor import AudioFrame


def make_frame(**kwargs):
    """Create an AudioFrame with sensible defaults."""
    defaults = {
        "timestamp": 100.0,
        "rms_energy": 0.01,
        "spectral_centroid_hz": 1500.0,
        "spectral_kurtosis": 2.8,
        "hf_lf_ratio": 0.1,
        "dominant_freq_hz": 440.0,
        "dominant_amp_db": -20.0,
        "spectrum_128": np.zeros(128, dtype=np.float32),
        "raw_audio": np.zeros(2048, dtype=np.int16),
    }
    defaults.update(kwargs)
    return AudioFrame(**defaults)


class TestThresholdDetector:
    def test_normal_returns_none(self, baseline_dict):
        det = ThresholdDetector(learning_period_s=0)
        frame = make_frame(
            rms_energy=0.01,       # exactly at baseline mean
            spectral_centroid_hz=1500.0,  # exactly at baseline mean
        )
        result = det.evaluate(frame, baseline_dict)
        assert result is None

    def test_rms_warning(self, baseline_dict):
        det = ThresholdDetector(
            sigma_warning=3.0, sigma_critical=5.0, learning_period_s=0,
        )
        # RMS at 3.5 sigma → warning
        frame = make_frame(
            rms_energy=0.01 + 3.5 * 0.002,  # mean + 3.5σ
        )
        result = det.evaluate(frame, baseline_dict)
        assert result is not None
        assert result.severity == "warning"
        assert "rms" in result.trigger_reason

    def test_rms_critical(self, baseline_dict):
        det = ThresholdDetector(
            sigma_warning=3.0, sigma_critical=5.0, learning_period_s=0,
        )
        # RMS at 6 sigma → critical
        frame = make_frame(
            rms_energy=0.01 + 6.0 * 0.002,
        )
        result = det.evaluate(frame, baseline_dict)
        assert result is not None
        assert result.severity == "critical"

    def test_kurtosis_critical(self, baseline_dict):
        det = ThresholdDetector(learning_period_s=0)
        # Kurtosis > 5 → critical regardless of sigma
        frame = make_frame(
            spectral_kurtosis=7.0,
            rms_energy=0.01,              # normal
            spectral_centroid_hz=1500.0,  # normal
        )
        result = det.evaluate(frame, baseline_dict)
        assert result is not None
        assert result.severity == "critical"
        assert result.trigger_reason == "kurtosis_exceeded"

    def test_learning_period_suppresses(self, baseline_dict):
        det = ThresholdDetector(learning_period_s=60)
        frame = make_frame(
            timestamp=10.0,          # only 10s elapsed
            rms_energy=1.0,           # way above baseline
        )
        det._start_time = 0.0        # simulate: started at t=0
        result = det.evaluate(frame, baseline_dict)
        assert result is None

    def test_learning_period_passed(self, baseline_dict):
        det = ThresholdDetector(learning_period_s=60)
        frame = make_frame(
            timestamp=70.0,           # 70s elapsed
            rms_energy=1.0,
        )
        det._start_time = 0.0
        result = det.evaluate(frame, baseline_dict)
        assert result is not None
        assert result.severity == "critical"

    def test_low_baseline_count_suppresses(self, baseline_dict):
        det = ThresholdDetector(learning_period_s=0)
        sparse = dict(baseline_dict, frame_count=3)
        frame = make_frame(rms_energy=1.0)
        result = det.evaluate(frame, sparse)
        assert result is None

    def test_sustained_escalation(self, baseline_dict):
        det = ThresholdDetector(
            sigma_warning=3.0,
            anomaly_sustain_s=30,
            learning_period_s=0,
        )
        # First frame: warning level
        frame = make_frame(
            timestamp=100.0,
            rms_energy=0.01 + 3.5 * 0.002,
        )
        result1 = det.evaluate(frame, baseline_dict)
        assert result1 is not None
        assert result1.severity == "warning"

        # 30s later, same level → sustained escalation to critical
        frame2 = make_frame(
            timestamp=200.0,  # 100s later, way past sustain threshold
            rms_energy=0.01 + 3.5 * 0.002,
        )
        result2 = det.evaluate(frame2, baseline_dict)
        assert result2 is not None
        assert result2.severity == "critical"
        assert "sustained" in result2.trigger_reason

    def test_centroid_warning(self, baseline_dict):
        det = ThresholdDetector(
            sigma_warning=3.0, sigma_critical=5.0, learning_period_s=0,
        )
        frame = make_frame(
            rms_energy=0.01,
            spectral_centroid_hz=1500.0 + 3.5 * 200.0,  # centroid + 3.5σ
        )
        result = det.evaluate(frame, baseline_dict)
        assert result is not None
        assert result.severity == "warning"
        # Should reference centroid
        assert "centroid" in result.trigger_reason

    def test_de_duplicate_same_severity(self, baseline_dict):
        """After first anomaly, same-severity frame should be suppressed."""
        det = ThresholdDetector(
            sigma_warning=3.0, sigma_critical=5.0, learning_period_s=0,
        )
        frame = make_frame(
            timestamp=100.0,
            rms_energy=0.01 + 3.5 * 0.002,
        )
        result1 = det.evaluate(frame, baseline_dict)
        assert result1 is not None

        # Immediate second call with same severity → suppressed
        result2 = det.evaluate(frame, baseline_dict)
        assert result2 is None
