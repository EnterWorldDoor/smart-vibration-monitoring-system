"""FFT spectral analysis, feature extraction, and baseline management.

Pure numpy/scipy module — no DB, MQTT, or file I/O.  Fully testable with
synthetic signals.  All processing in the callback must complete in < 1 ms
(2048-point RFFT + 5 features + 128-bin downsample on A73/A55).

Design:
  AudioProcessor.process_block()    — called from portaudio callback (fast path)
  AudioProcessor.update_baseline()  — called from main thread (Welford online)
  AudioProcessor.baseline          — read-only property, consumed by detector
"""

from dataclasses import dataclass, field
from typing import Optional

import numpy as np
from scipy import special


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class AudioFrame:
    """Single FFT block output from the audio callback."""
    timestamp: float                  # time.monotonic() in callback
    rms_energy: float
    spectral_centroid_hz: float
    spectral_kurtosis: float
    hf_lf_ratio: float                # 2-8kHz energy / 0-500Hz energy
    dominant_freq_hz: float
    dominant_amp_db: float
    spectrum_128: np.ndarray          # shape (128,) float32, log-spaced avg
    raw_audio: np.ndarray             # shape (block_size,) int16, for ring buff


# ---------------------------------------------------------------------------
# Online running statistics (Welford's algorithm)
# ---------------------------------------------------------------------------

class BaselineStats:
    """O(1) running mean and variance.  No historical storage."""

    def __init__(self):
        self.mean: float = 0.0
        self.m2: float = 0.0          # sum of squared differences
        self.count: int = 0

    def update(self, value: float) -> None:
        self.count += 1
        delta = value - self.mean
        self.mean += delta / self.count
        delta2 = value - self.mean
        self.m2 += delta * delta2

    def std(self) -> float:
        if self.count < 2:
            return 0.0
        return float(np.sqrt(self.m2 / (self.count - 1)))

    def variance(self) -> float:
        if self.count < 2:
            return 0.0
        return self.m2 / (self.count - 1)

    def sigma_level(self, value: float) -> float:
        s = self.std()
        if s < 1e-15:
            return 0.0
        return (value - self.mean) / s

    def reset(self) -> None:
        self.mean = 0.0
        self.m2 = 0.0
        self.count = 0


# ---------------------------------------------------------------------------
# Audio processor
# ---------------------------------------------------------------------------

class AudioProcessor:
    """FFT + 5 acoustic features + Welford baseline + 128-bin spectrum."""

    def __init__(
        self,
        sample_rate: int = 16000,
        block_size: int = 2048,
        baseline_window_s: int = 60,
        learning_period_s: int = 60,
    ):
        self.sample_rate = sample_rate
        self.block_size = block_size

        self._start_time: Optional[float] = None
        self._window = np.hanning(block_size).astype(np.float32)
        self._freq_bins = np.fft.rfftfreq(block_size, d=1.0 / sample_rate).astype(np.float32)

        # 128-bin log-spaced edges (0 … Nyquist)
        self._spectrum_edges = self._compute_log_edges(128, sample_rate / 2)

        # Baseline accumulators
        self._bl_rms = BaselineStats()
        self._bl_centroid = BaselineStats()
        self._bl_kurtosis = BaselineStats()
        self._bl_hflf = BaselineStats()

    # --- Fast path (callback) ------------------------------------------------

    def process_block(self, raw_samples: np.ndarray) -> AudioFrame:
        """FFT + 5 features + 128-bin downsample.  Must be fast (<1 ms)."""
        # Normalise int16 → float32 [-1, 1]
        signal = raw_samples.astype(np.float32) / 32768.0
        signal *= self._window

        # RFFT → magnitude spectrum
        mag = np.abs(np.fft.rfft(signal)).astype(np.float32)

        # ── 1. RMS energy ──
        rms = float(np.sqrt(np.mean(np.square(signal))))

        # ── 2. Spectral centroid (Hz) ──
        mag_sum = float(np.sum(mag))
        centroid = float(np.dot(self._freq_bins, mag) / max(mag_sum, 1e-15))

        # ── 3. Spectral kurtosis (on magnitude distribution) ──
        diff = mag - (mag_sum / len(mag))
        m2 = np.mean(np.square(diff))
        m4 = np.mean(np.power(diff, 4.0))
        kurt = float(m4 / max(m2 * m2, 1e-15)) if m2 > 1e-15 else 1.0

        # ── 4. HF/LF energy ratio (2-8 kHz / 0-500 Hz) ──
        hf_mask = (self._freq_bins >= 2000.0) & (self._freq_bins <= 8000.0)
        lf_mask = self._freq_bins <= 500.0
        e_hf = float(np.sum(mag[hf_mask]))
        e_lf = float(np.sum(mag[lf_mask]))
        hflf = e_hf / max(e_lf, 1e-15)

        # ── 5. Dominant frequency + amplitude ──
        peak_idx = int(np.argmax(mag))
        dom_freq = float(self._freq_bins[peak_idx])
        dom_amp = float(20.0 * np.log10(max(mag[peak_idx], 1e-15)))

        # ── 128-bin log-spaced downsampled spectrum ──
        spec128 = self._downsample_spectrum(mag)

        return AudioFrame(
            timestamp=0.0,  # filled by caller (capture callback)
            rms_energy=rms,
            spectral_centroid_hz=centroid,
            spectral_kurtosis=kurt,
            hf_lf_ratio=hflf,
            dominant_freq_hz=dom_freq,
            dominant_amp_db=dom_amp,
            spectrum_128=spec128,
            raw_audio=raw_samples.copy(),
        )

    # --- Baseline update (main thread) ---------------------------------------

    def update_baseline(self, frame: AudioFrame) -> None:
        """Welford online update.  Only called when not in anomaly."""
        now = frame.timestamp
        if self._start_time is None:
            self._start_time = now
        self._bl_rms.update(frame.rms_energy)
        self._bl_centroid.update(frame.spectral_centroid_hz)
        self._bl_kurtosis.update(frame.spectral_kurtosis)
        self._bl_hflf.update(frame.hf_lf_ratio)

    @property
    def baseline(self) -> dict:
        elapsed = True
        if self._start_time is not None:
            # Learning period tracked by detector, not here
            pass
        return {
            "rms_mean": self._bl_rms.mean,
            "rms_std": self._bl_rms.std(),
            "centroid_mean": self._bl_centroid.mean,
            "centroid_std": self._bl_centroid.std(),
            "kurtosis_mean": self._bl_kurtosis.mean,
            "kurtosis_std": self._bl_kurtosis.std(),
            "hflf_mean": self._bl_hflf.mean,
            "hflf_std": self._bl_hflf.std(),
            "frame_count": self._bl_rms.count,
        }

    def reset_baseline(self) -> None:
        self._start_time = None
        self._bl_rms.reset()
        self._bl_centroid.reset()
        self._bl_kurtosis.reset()
        self._bl_hflf.reset()

    # --- Internals -----------------------------------------------------------

    def _downsample_spectrum(self, mag: np.ndarray) -> np.ndarray:
        """Average magnitude into 128 log-spaced frequency bins."""
        result = np.zeros(128, dtype=np.float32)
        for i in range(128):
            lo = int(self._spectrum_edges[i])
            hi = int(self._spectrum_edges[i + 1])
            if hi > lo:
                result[i] = float(np.mean(mag[lo:hi]))
        return result

    @staticmethod
    def _compute_log_edges(num_bins: int, nyquist: float) -> np.ndarray:
        """Compute bin edges spaced logarithmically from 1 Hz to nyquist."""
        return np.logspace(0.0, np.log10(max(nyquist, 2.0)), num_bins + 1)
