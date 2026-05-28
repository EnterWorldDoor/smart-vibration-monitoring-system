"""Pluggable anomaly detection for audio frames.

MVP: ThresholdDetector — compares frame features against BaselineStats
      using multi-sigma rules.  Phase 2: AutoencoderDetector.

Design:
  Detector.evaluate(frame, baseline) → Optional[AnomalyResult]
  None = normal, AnomalyResult = anomaly detected.
  Internal state: sustained-anomaly timer for escalation.
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Optional

from src.audio.processor import AudioFrame


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class AnomalyResult:
    severity: str          # "warning" | "critical"
    trigger_reason: str    # e.g. "rms_energy_exceeded", "kurtosis_exceeded"
    sigma_level: float
    features: dict = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Abstract interface
# ---------------------------------------------------------------------------

class Detector(ABC):
    """Pluggable anomaly detector.  Implementations must be thread-safe
    (only called from the main thread, but state may be persisted)."""

    @abstractmethod
    def evaluate(
        self, frame: AudioFrame, baseline: dict
    ) -> Optional[AnomalyResult]:
        ...


# ---------------------------------------------------------------------------
# Threshold detector (MVP)
# ---------------------------------------------------------------------------

class ThresholdDetector(Detector):
    """Multi-sigma threshold detector with sustained-anomaly escalation."""

    def __init__(
        self,
        sigma_warning: float = 3.0,
        sigma_critical: float = 5.0,
        anomaly_sustain_s: int = 30,
        learning_period_s: int = 60,
    ):
        self._sigma_warning = sigma_warning
        self._sigma_critical = sigma_critical
        self._anomaly_sustain_s = anomaly_sustain_s
        self._learning_period_s = learning_period_s

        self._start_time: Optional[float] = None
        self._anomaly_start: Optional[float] = None  # monotonic time
        self._last_severity: Optional[str] = None

    # ------------------------------------------------------------------

    def evaluate(
        self, frame: AudioFrame, baseline: dict
    ) -> Optional[AnomalyResult]:
        now = frame.timestamp

        # Track first-seen for learning period
        if self._start_time is None:
            self._start_time = now
        elapsed = now - self._start_time

        # Learning period: no triggers
        if elapsed < self._learning_period_s:
            return None

        # Not enough baseline samples → no triggers
        bc = baseline.get("frame_count", 0)
        if bc < 10:
            return None

        # ── Compute sigma levels ──
        def _sigma(val, key_mean, key_std):
            mean = baseline.get(key_mean, 0.0)
            std_ = baseline.get(key_std, 0.0)
            if std_ < 1e-15:
                return 0.0
            return (val - mean) / std_

        sigma_rms = _sigma(
            frame.rms_energy, "rms_mean", "rms_std"
        )
        sigma_centroid = _sigma(
            frame.spectral_centroid_hz, "centroid_mean", "centroid_std"
        )

        # Kurtosis > 5.0 is intrinsically impulsive (critical regardless of baseline)
        kurtosis = frame.spectral_kurtosis

        # ── Determine severity ──
        severity: Optional[str] = None
        reason: Optional[str] = None
        max_sigma: float = 0.0

        if kurtosis > 5.0:
            severity = "critical"
            reason = "kurtosis_exceeded"
            max_sigma = 0.0
        elif sigma_rms >= self._sigma_critical:
            severity = "critical"
            reason = "rms_energy_exceeded"
            max_sigma = sigma_rms
        elif sigma_centroid >= self._sigma_critical:
            severity = "critical"
            reason = "spectral_centroid_exceeded"
            max_sigma = sigma_centroid
        elif sigma_rms >= self._sigma_warning:
            severity = "warning"
            reason = "rms_energy_exceeded"
            max_sigma = sigma_rms
        elif sigma_centroid >= self._sigma_warning:
            severity = "warning"
            reason = "spectral_centroid_exceeded"
            max_sigma = sigma_centroid

        # ── Sustained anomaly escalation ──
        if severity is not None:
            if self._anomaly_start is None:
                self._anomaly_start = now
            elif (now - self._anomaly_start) > self._anomaly_sustain_s:
                severity = "critical"
                reason = (reason or "") + "_sustained"
        else:
            self._anomaly_start = None

        # ── Return ──
        if severity is None:
            self._last_severity = None
            return None

        # De-duplicate: only emit one anomaly start event per anomaly period
        if self._anomaly_start is not None and self._last_severity is not None:
            # Already tracking.  Only emit new event on escalation or start.
            if severity == self._last_severity:
                return None  # suppress, already reported

        self._last_severity = severity
        return AnomalyResult(
            severity=severity,
            trigger_reason=reason or "unknown",
            sigma_level=max_sigma,
            features={
                "rms_energy": frame.rms_energy,
                "spectral_centroid_hz": frame.spectral_centroid_hz,
                "spectral_kurtosis": kurtosis,
                "hf_lf_ratio": frame.hf_lf_ratio,
                "dominant_freq_hz": frame.dominant_freq_hz,
                "dominant_amp_db": frame.dominant_amp_db,
            },
        )
