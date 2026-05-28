"""Shared test fixtures for audio-monitor."""

import os
import tempfile
from pathlib import Path

import numpy as np
import pytest

# Ensure the src/ directory is importable
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))


@pytest.fixture
def tmp_storage():
    """Temporary storage directory that cleans up after test."""
    with tempfile.TemporaryDirectory() as d:
        yield d


@pytest.fixture
def sample_rate():
    return 16000


@pytest.fixture
def block_size():
    return 2048


@pytest.fixture
def synthetic_sine_440hz(sample_rate, block_size):
    """Generate a block of 440 Hz sine wave at amplitude 0.5."""
    t = np.arange(block_size) / sample_rate
    signal = (0.5 * 32767 * np.sin(2 * np.pi * 440 * t)).astype(np.int16)
    return signal


@pytest.fixture
def synthetic_noise(sample_rate, block_size):
    """Generate a block of white noise."""
    rng = np.random.RandomState(42)
    return (rng.randn(block_size) * 1000).astype(np.int16)


@pytest.fixture
def synthetic_impulse(sample_rate, block_size):
    """Generate a block with a single impulse (bearing impact simulation)."""
    signal = np.zeros(block_size, dtype=np.int16)
    signal[512] = 30000
    signal[1000] = 28000
    signal[1500] = 29000
    return signal


@pytest.fixture
def baseline_dict():
    """Well-formed baseline dict with known values."""
    return {
        "rms_mean": 0.01,
        "rms_std": 0.002,
        "centroid_mean": 1500.0,
        "centroid_std": 200.0,
        "kurtosis_mean": 2.8,
        "kurtosis_std": 0.5,
        "hflf_mean": 0.1,
        "hflf_std": 0.02,
        "frame_count": 500,
    }
