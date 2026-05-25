"""Layered aggregation: device-level → motor-level (DE/NDE) → site-level.

Architecture (from CONTEXT.md Decision 5):
  Layer 1: Per-device Autoencoder anomaly score + trend analysis
  Layer 2: DE/NDE comparison → motor health score (weighted)
  Layer 3: Multi-motor ranking (Phase 2)
"""

from dataclasses import dataclass, field
from typing import Optional

import structlog

from src.inference.trend_analysis import TrendResult

logger = structlog.get_logger(__name__)


@dataclass
class MotorHealth:
    motor_id: str
    health_score: float = 100.0  # 0-100
    anomaly_detected: bool = False
    de_anomaly_score: Optional[float] = None
    nde_anomaly_score: Optional[float] = None
    de_nde_ratio: Optional[float] = None
    severity: str = "NORMAL"  # NORMAL, WARNING, CRITICAL
    summary: str = ""
    details: dict = field(default_factory=dict)


def compute_motor_health(
    motor_id: str,
    de_trend: TrendResult,
    nde_trend: Optional[TrendResult],
    de_anomaly_score: Optional[float],
    nde_anomaly_score: Optional[float],
) -> MotorHealth:
    health = MotorHealth(motor_id=motor_id)

    # Weight contributions to health score reduction
    deductions = 0.0

    # Autoencoder anomaly (weight: 40%)
    if de_anomaly_score and de_anomaly_score > 0:
        health.de_anomaly_score = de_anomaly_score
        severity = min(de_anomaly_score * 10, 1.0)  # normalize
        deductions += severity * 40
        health.anomaly_detected = True

    if nde_anomaly_score and nde_anomaly_score > 0:
        health.nde_anomaly_score = nde_anomaly_score
        severity = min(nde_anomaly_score * 10, 1.0)
        deductions += severity * 10  # NDE is weighted less than DE

    # Trend warnings (weight: 35%)
    trend_issues = 0
    if de_trend.rms_slope_warning:
        trend_issues += 1
    if de_trend.freq_drift_warning:
        trend_issues += 1
    if de_trend.crest_factor_warning:
        trend_issues += 1
    if de_trend.temp_correlation_warning:
        trend_issues += 1
    deductions += min(trend_issues * 8, 35)

    # DE/NDE ratio (weight: 25%)
    if de_trend.de_nde_ratio and de_trend.de_nde_ratio_warning:
        health.de_nde_ratio = de_trend.de_nde_ratio
        ratio_deviation = abs(de_trend.de_nde_ratio - 1.0)
        deductions += min(ratio_deviation * 50, 25)

    health.health_score = round(max(100.0 - deductions, 0.0), 1)

    # Severity classification
    if health.health_score >= 80:
        health.severity = "NORMAL"
    elif health.health_score >= 50:
        health.severity = "WARNING"
    else:
        health.severity = "CRITICAL"

    # Summary
    parts = [f"Motor {motor_id}: health={health.health_score:.0f}"]
    if health.anomaly_detected:
        parts.append("anomaly detected")
    if de_trend.warnings:
        parts.append(f"{len(de_trend.warnings)} trend warnings")
    health.summary = "; ".join(parts)
    health.details = {
        "trend_warnings": de_trend.warnings,
        "de_anomaly_score": de_anomaly_score,
        "nde_anomaly_score": nde_anomaly_score,
        "de_nde_ratio": de_trend.de_nde_ratio,
        "rms_slope": de_trend.rms_slope,
        "freq_drift_std": de_trend.freq_drift_std,
        "crest_factor_slope": de_trend.crest_factor_slope,
        "band_energy_shift": de_trend.band_energy_shift,
        "temp_vib_correlation": de_trend.temp_vib_correlation,
    }

    return health
