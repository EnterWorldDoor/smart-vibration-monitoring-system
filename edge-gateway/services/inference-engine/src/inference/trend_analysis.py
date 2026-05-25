"""Statistical trend analysis — 6 indicators on sliding windows.

Indicators (from CONTEXT.md Decision 9):
  1. RMS trend slope (linear regression on overall_rms)
  2. Peak frequency drift (std dev of peak_frequency)
  3. Temperature-vibration correlation (Pearson r: RMS vs temperature)
  4. DE/NDE RMS ratio (deviation from 1.0)
  5. Crest factor trend (linear regression slope)
  6. Band energy migration (high-freq 200-400Hz energy share trend)
"""

from dataclasses import dataclass, field
from typing import Optional

import numpy as np
from scipy import stats
import structlog

from src.config import TrendConfig

logger = structlog.get_logger(__name__)


@dataclass
class TrendResult:
    rms_slope: float = 0.0
    rms_slope_warning: bool = False
    freq_drift_std: float = 0.0
    freq_drift_warning: bool = False
    temp_vib_correlation: float = 0.0
    temp_correlation_warning: bool = False
    de_nde_ratio: Optional[float] = None
    de_nde_ratio_warning: bool = False
    crest_factor_slope: float = 0.0
    crest_factor_warning: bool = False
    band_energy_shift: float = 0.0
    band_energy_warning: bool = False
    overall_status: str = "normal"
    warnings: list = field(default_factory=list)


class TrendAnalyzer:
    def __init__(self, cfg: TrendConfig):
        self.cfg = cfg

    def analyze(self, vibration_rows: list, dual_channel_rows: list,
                feature_rows: list) -> TrendResult:
        result = TrendResult()

        n = len(vibration_rows)
        # Only return early if all data sources are empty
        if n < 5 and not dual_channel_rows and not feature_rows:
            return result

        # --- 1. RMS trend slope ---
        rms_vals = np.array([r["overall_rms"] or 0.0 for r in vibration_rows])
        if len(rms_vals) >= 5:
            x = np.arange(len(rms_vals))
            slope = stats.linregress(x, rms_vals).slope
            result.rms_slope = float(slope)
            if slope > self.cfg.rms_slope_warn:
                result.rms_slope_warning = True
                result.warnings.append(f"RMS rising: {slope:.3f} mm/s per point")

        # --- 2. Peak frequency drift ---
        freq_vals = np.array([r["peak_frequency_hz"] or 0.0 for r in vibration_rows])
        if len(freq_vals) >= 5:
            result.freq_drift_std = float(np.std(freq_vals))
            if result.freq_drift_std > self.cfg.freq_drift_warn:
                result.freq_drift_warning = True
                result.warnings.append(f"Freq drift: σ={result.freq_drift_std:.1f} Hz")

        # --- 3. Temperature-vibration correlation ---
        if feature_rows and len(feature_rows) >= 10:
            temps = np.array([r.get("feat_temperature_c", 25.0) or 25.0
                             for r in feature_rows])
            rms_from_features = np.array([r.get("feat_overall_rms", 0.0) or 0.0
                                          for r in feature_rows])
            if np.std(temps) > 0.5 and np.std(rms_from_features) > 1e-6:
                pearson_r, _ = stats.pearsonr(temps, rms_from_features)
                result.temp_vib_correlation = float(pearson_r)
                if abs(pearson_r) > self.cfg.temp_correlation_warn:
                    result.temp_correlation_warning = True
                    result.warnings.append(f"Temp-Vib correlation: r={pearson_r:.2f}")

        # --- 4. DE/NDE RMS ratio ---
        if dual_channel_rows:
            ratios = [r["rms_ratio"] or 1.0 for r in dual_channel_rows
                     if r.get("rms_ratio") is not None]
            if ratios:
                avg_ratio = float(np.mean(ratios))
                result.de_nde_ratio = avg_ratio
                if abs(avg_ratio - 1.0) > self.cfg.de_nde_ratio_warn:
                    result.de_nde_ratio_warning = True
                    result.warnings.append(f"DE/NDE ratio deviates: {avg_ratio:.2f}")

        # --- 5. Crest factor trend ---
        if feature_rows and len(feature_rows) >= 5:
            cf_vals = np.array([r.get("feat_crest_factor_x", 0.0) or 0.0
                               for r in feature_rows])
            if len(cf_vals) >= 5:
                slope_cf = stats.linregress(np.arange(len(cf_vals)), cf_vals).slope
                result.crest_factor_slope = float(slope_cf)
                if slope_cf > self.cfg.crest_factor_slope_warn:
                    result.crest_factor_warning = True
                    result.warnings.append(f"Crest factor rising: {slope_cf:.4f}/point")

        # --- 6. Band energy migration (200-400Hz share) ---
        if feature_rows and len(feature_rows) >= 5:
            band_hi_share = []
            for row in feature_rows:
                total = sum(row.get(f"feat_band_energy_x_{i}", 0.0) or 0.0
                           for i in range(8))
                hi = (row.get("feat_band_energy_x_6", 0.0) or 0.0) + \
                     (row.get("feat_band_energy_x_7", 0.0) or 0.0)
                if total > 1e-10:
                    band_hi_share.append(hi / total)
            if len(band_hi_share) >= 5:
                slope_hi = stats.linregress(np.arange(len(band_hi_share)),
                                        band_hi_share).slope
                result.band_energy_shift = float(slope_hi)
                if slope_hi > self.cfg.band_energy_shift_warn / 86400:
                    result.band_energy_warning = True
                    result.warnings.append(f"HF energy rising: {slope_hi:.6f}/point")

        # --- Overall status ---
        if result.warnings:
            critical_indicators = [
                result.rms_slope_warning,
                result.freq_drift_warning,
                result.de_nde_ratio_warning,
            ]
            if sum(critical_indicators) >= 2:
                result.overall_status = "critical"
            else:
                result.overall_status = "warning"

        return result
