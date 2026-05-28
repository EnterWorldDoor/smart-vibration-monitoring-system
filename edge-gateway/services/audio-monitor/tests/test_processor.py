"""Test FFT processing, feature extraction, and baseline statistics.

Uses synthetic signals (sine, noise, impulse) to verify correctness of
the 5 acoustic features and Welford online baseline algorithm.
"""

import numpy as np
import pytest

from src.audio.processor import (
    AudioProcessor, AudioFrame, BaselineStats,
)


class TestBaselineStats:
    def test_welford_convergence(self):
        bs = BaselineStats()
        for _ in range(1000):
            bs.update(5.0)
        assert abs(bs.mean - 5.0) < 0.001
        assert bs.std() < 0.001

    def test_welford_known_sequence(self):
        bs = BaselineStats()
        for v in [1.0, 2.0, 3.0, 4.0, 5.0]:
            bs.update(v)
        assert abs(bs.mean - 3.0) < 0.001
        # Sample std of [1,2,3,4,5] = sqrt(2.5) ≈ 1.581
        assert abs(bs.std() - 1.5811388300841898) < 0.01

    def test_sigma_level(self):
        bs = BaselineStats()
        for v in [10.0, 10.0, 10.0, 10.0]:
            bs.update(v)
        # With zero variance, sigma = 0
        assert bs.sigma_level(10.0) == 0.0
        assert bs.sigma_level(100.0) == 0.0

    def test_sigma_level_with_variance(self):
        bs = BaselineStats()
        for _ in range(100):
            bs.update(np.random.normal(5.0, 1.0))
        s = bs.sigma_level(8.0)
        # 8.0 is ~3 sigma above 5.0
        assert s > 1.5

    def test_reset(self):
        bs = BaselineStats()
        bs.update(1.0)
        bs.update(2.0)
        bs.reset()
        assert bs.count == 0
        assert bs.mean == 0.0


class TestAudioProcessor:
    def test_sine_440hz(self, synthetic_sine_440hz, block_size, sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        frame = proc.process_block(synthetic_sine_440hz)

        # Dominant frequency should be near 440 Hz
        # (within 1 FFT bin = sample_rate / block_size = 7.8 Hz)
        assert abs(frame.dominant_freq_hz - 440.0) < 10.0

        # RMS of 0.5 amplitude sine after Hann window ≈ 0.22
        assert abs(frame.rms_energy - 0.22) < 0.05

        # Spectral centroid near 440 Hz
        assert abs(frame.spectral_centroid_hz - 440.0) < 50.0

    def test_noise_kurtosis(self, synthetic_noise, block_size, sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        frame = proc.process_block(synthetic_noise)

        # White noise spectral kurtosis ≈ 3.0 (Gaussian)
        assert 1.5 < frame.spectral_kurtosis < 6.0

    def test_impulse_handled_without_crash(self, synthetic_impulse,
                                             block_size, sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        frame = proc.process_block(synthetic_impulse)

        # Impulses produce many high-frequency bins — HF/LF ratio spikes
        assert frame.hf_lf_ratio > 1.0
        assert not np.isnan(frame.spectral_kurtosis)

    def test_zero_signal(self, block_size, sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        signal = np.zeros(block_size, dtype=np.int16)
        frame = proc.process_block(signal)

        assert frame.rms_energy == 0.0
        assert frame.spectral_centroid_hz == 0.0
        assert frame.dominant_amp_db < -100  # very quiet

    def test_spectrum_128_shape(self, synthetic_sine_440hz, block_size,
                                 sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        frame = proc.process_block(synthetic_sine_440hz)

        assert frame.spectrum_128.shape == (128,)
        assert frame.spectrum_128.dtype == np.float32

    def test_dc_offset(self, block_size, sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        signal = np.full(block_size, 10000, dtype=np.int16)
        frame = proc.process_block(signal)

        # DC offset after Hann window: 10000/32768 * sqrt(mean(hann²)) ≈ 0.187
        assert abs(frame.rms_energy - 0.187) < 0.03
        # Dominant freq = 0 Hz
        assert frame.dominant_freq_hz < 10.0

    def test_update_baseline(self, synthetic_sine_440hz, block_size,
                              sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        frame = proc.process_block(synthetic_sine_440hz)
        frame.timestamp = 100.0

        proc.update_baseline(frame)
        b = proc.baseline
        assert b["frame_count"] == 1
        assert b["rms_mean"] == frame.rms_energy

    def test_reset_baseline(self, synthetic_sine_440hz, block_size,
                             sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        frame = proc.process_block(synthetic_sine_440hz)
        frame.timestamp = 0.0

        proc.update_baseline(frame)
        proc.reset_baseline()
        assert proc.baseline["frame_count"] == 0

    def test_raw_audio_preserved(self, synthetic_sine_440hz, block_size,
                                  sample_rate):
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        frame = proc.process_block(synthetic_sine_440hz)

        assert frame.raw_audio.shape == (block_size,)
        assert frame.raw_audio.dtype == np.int16
        assert np.array_equal(frame.raw_audio, synthetic_sine_440hz)

    def test_hf_lf_ratio_band_boundaries(self, block_size, sample_rate):
        """Inject energy at 4000 Hz and verify hf_lf_ratio > 0."""
        proc = AudioProcessor(sample_rate=sample_rate, block_size=block_size)
        t = np.arange(block_size) / sample_rate
        signal = (0.5 * 32767 * np.sin(2 * np.pi * 4000 * t)).astype(np.int16)
        frame = proc.process_block(signal)

        # Energy at 4000 Hz falls in HF band (2-8kHz)
        assert frame.hf_lf_ratio > 0.1
