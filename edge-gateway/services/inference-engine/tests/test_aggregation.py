"""Test motor health aggregation logic."""

from src.inference.aggregation import compute_motor_health, MotorHealth
from src.inference.trend_analysis import TrendResult


def test_normal_motor_health():
    trend = TrendResult(overall_status="normal")
    health = compute_motor_health("motor01", trend, None, None, None)
    assert health.health_score >= 95.0
    assert health.severity == "NORMAL"
    assert not health.anomaly_detected


def test_anomaly_reduces_health():
    trend = TrendResult(overall_status="normal")
    health = compute_motor_health("motor01", trend, None, 0.5, None)
    assert health.health_score < 90.0
    assert health.anomaly_detected


def test_warnings_reduce_health():
    trend = TrendResult(
        overall_status="warning",
        rms_slope_warning=True,
        freq_drift_warning=True,
        crest_factor_warning=True,
        warnings=["RMS rising: 0.1", "Freq drift", "Crest rising"],
    )
    health = compute_motor_health("motor01", trend, None, None, None)
    assert health.health_score < 90.0


def test_de_nde_ratio_reduces_health():
    trend = TrendResult(
        overall_status="warning",
        de_nde_ratio=1.8,
        de_nde_ratio_warning=True,
    )
    health = compute_motor_health("motor01", trend, None, None, None)
    assert health.health_score < 90.0
    assert health.de_nde_ratio == 1.8


def test_critical_health():
    trend = TrendResult(
        overall_status="critical",
        rms_slope_warning=True,
        freq_drift_warning=True,
        de_nde_ratio=2.0,
        de_nde_ratio_warning=True,
        warnings=["Multiple faults"],
    )
    health = compute_motor_health("motor01", trend, None, 1.0, None)
    assert health.severity == "CRITICAL"
    assert health.health_score < 50.0


def test_health_score_bounds():
    """Health score should always be in [0, 100]."""
    trend = TrendResult(overall_status="normal")
    # Extreme values
    health = compute_motor_health("m", trend, None, 10.0, 5.0)
    assert 0.0 <= health.health_score <= 100.0
