"""Test PreTriggerBuffer ring buffer, WAV writing, and file rotation."""

import os
import time
import wave

import numpy as np
import pytest

from src.storage.file_store import (
    PreTriggerBuffer, build_wav_path, build_baseline_path,
    ensure_dir, write_wav, rotate_expired_anomalies, rotate_baselines,
)


class TestPreTriggerBuffer:
    def test_maxlen(self):
        buf = PreTriggerBuffer(duration_s=3.0, sample_rate=16000,
                                block_size=2048)
        # 3s * 16000 / 2048 ≈ 23.44 → int(23.44) = 23, +1 = 24
        assert buf._max_blocks == 24

    def test_push_and_drain(self):
        buf = PreTriggerBuffer(duration_s=3.0, sample_rate=16000,
                                block_size=2048)
        for i in range(50):
            block = np.full(2048, i, dtype=np.int16)
            buf.push(block)

        flat = buf.drain()
        # Only last max_blocks (24) are preserved
        assert len(flat) == 24 * 2048
        # First value should be from block index 26 (0-indexed, 50 pushed, last 24 kept)
        assert flat[0] == 26

    def test_drain_empty(self):
        buf = PreTriggerBuffer(duration_s=3.0, sample_rate=16000,
                                block_size=2048)
        flat = buf.drain()
        assert len(flat) == 0

    def test_clear(self):
        buf = PreTriggerBuffer(duration_s=3.0, sample_rate=16000,
                                block_size=2048)
        buf.push(np.zeros(2048, dtype=np.int16))
        buf.clear()
        assert buf.num_blocks == 0


class TestWavWrite:
    def test_write_and_verify(self, tmp_storage):
        data = np.arange(4096, dtype=np.int16)
        path = os.path.join(tmp_storage, "test.wav")

        ok = write_wav(path, data, 16000)
        assert ok
        assert os.path.exists(path)

        # Verify header
        with wave.open(path, "rb") as wf:
            assert wf.getnchannels() == 1
            assert wf.getsampwidth() == 2
            assert wf.getframerate() == 16000
            assert wf.getnframes() == 4096
            frames = np.frombuffer(wf.readframes(4096), dtype=np.int16)
            assert np.array_equal(frames, data)

    def test_atomic_write_no_tmp_left(self, tmp_storage):
        data = np.zeros(100, dtype=np.int16)
        path = os.path.join(tmp_storage, "atomic.wav")
        write_wav(path, data, 16000)
        assert not os.path.exists(path + ".tmp")

    def test_float_converted_to_int16(self, tmp_storage):
        data = np.array([0.1, -0.2, 0.3], dtype=np.float64)
        path = os.path.join(tmp_storage, "float.wav")
        ok = write_wav(path, data, 16000)
        assert ok

        with wave.open(path, "rb") as wf:
            frames = np.frombuffer(wf.readframes(3), dtype=np.int16)
        # Float was converted to int16
        assert frames.dtype == np.int16

    def test_build_wav_path(self):
        from datetime import datetime, timezone
        ts = datetime(2026, 5, 28, 10, 30, 15, 123456, tzinfo=timezone.utc)
        path = build_wav_path("/var/lib/edgevib/audio", "audio01", ts)
        assert "audio01_20260528T103015.wav" in path
        assert "anomalies" in path


class TestRotation:
    def test_rotate_anomalies_count(self, tmp_storage):
        anomal_dir = os.path.join(tmp_storage, "anomalies")
        ensure_dir(anomal_dir)

        # Create 10 wav files
        for i in range(10):
            path = os.path.join(anomal_dir, f"anom_{i}.wav")
            with open(path, "wb") as f:
                f.write(b"dummy")
            time.sleep(0.01)  # ensure different mtimes

        deleted = rotate_expired_anomalies(tmp_storage, max_files=5)
        assert deleted == 5
        remaining = [f for f in os.listdir(anomal_dir) if f.endswith(".wav")]
        assert len(remaining) == 5

    def test_rotate_baselines(self, tmp_storage):
        bl_dir = os.path.join(tmp_storage, "baselines")
        ensure_dir(bl_dir)

        for i in range(10):
            path = os.path.join(bl_dir, f"base_{i}.wav")
            with open(path, "wb") as f:
                f.write(b"dummy")
            time.sleep(0.01)

        deleted = rotate_baselines(tmp_storage, max_files=3)
        assert deleted == 7
        remaining = [f for f in os.listdir(bl_dir) if f.endswith(".wav")]
        assert len(remaining) == 3

    def test_rotate_empty_directory(self, tmp_storage):
        ensure_dir(os.path.join(tmp_storage, "anomalies"))
        ensure_dir(os.path.join(tmp_storage, "baselines"))
        deleted = rotate_expired_anomalies(tmp_storage)
        assert deleted == 0
        deleted = rotate_baselines(tmp_storage)
        assert deleted == 0
