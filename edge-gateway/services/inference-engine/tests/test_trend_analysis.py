"""Test trend analysis indicators."""

from src.config import TrendConfig
from src.inference.trend_analysis import TrendAnalyzer


def test_flat_data_no_warnings(sample_vibration_rows):
    cfg = TrendConfig()
    analyzer = TrendAnalyzer(cfg)
    result = analyzer.analyze(sample_vibration_rows, [], [])
    assert result.overall_status == "normal"
    assert len(result.warnings) == 0


def test_rising_rms_triggers_warning(sample_vibration_rows_rising):
    cfg = TrendConfig(rms_slope_warn=0.05)
    analyzer = TrendAnalyzer(cfg)
    result = analyzer.analyze(sample_vibration_rows_rising, [], [])
    assert result.rms_slope_warning
    assert "RMS rising" in result.warnings[0]


def test_freq_drift_warning():
    cfg = TrendConfig(freq_drift_warn=5.0)
    analyzer = TrendAnalyzer(cfg)
    # Create data with high frequency variance
    import numpy as np
    rows = []
    for i in range(30):
        rows.append({
            "time": f"2026-05-25T10:00:{i:02d}Z",
            "rms_x": 0.005, "rms_y": 0.005, "rms_z": 0.005,
            "overall_rms": 0.01,
            "peak_frequency_hz": float(100 + np.random.normal(0, 20)),
            "peak_amplitude_g": 0.02,
        })
    result = analyzer.analyze(rows, [], [])
    assert result.freq_drift_warning


def test_de_nde_ratio_warning():
    cfg = TrendConfig(de_nde_ratio_warn=0.3)
    analyzer = TrendAnalyzer(cfg)
    dual_rows = [{"rms_ratio": 1.5}] * 30  # 50% deviation
    result = analyzer.analyze([], dual_rows, [])
    assert result.de_nde_ratio_warning
    assert abs(result.de_nde_ratio - 1.5) < 1e-6


def test_critical_status_with_multiple_warnings():
    cfg = TrendConfig(rms_slope_warn=0.01, freq_drift_warn=3.0,
                      de_nde_ratio_warn=0.1)
    analyzer = TrendAnalyzer(cfg)
    # Rising RMS data
    import numpy as np
    rows = []
    for i in range(30):
        rows.append({
            "time": f"2026-05-25T10:00:{i:02d}Z",
            "rms_x": 0.005, "rms_y": 0.005, "rms_z": 0.005,
            "overall_rms": float(1.0 + i * 0.1),
            "peak_frequency_hz": float(100 + i * 2),
            "peak_amplitude_g": 0.02,
        })
    dual_rows = [{"rms_ratio": 1.8}] * 30
    result = analyzer.analyze(rows, dual_rows, [])
    assert result.overall_status == "critical"


def test_empty_data():
    cfg = TrendConfig()
    analyzer = TrendAnalyzer(cfg)
    result = analyzer.analyze([], [], [])
    assert result.overall_status == "normal"
    assert result.rms_slope == 0.0
