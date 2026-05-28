"""WAV file storage with ring buffer and automatic rotation.

PreTriggerBuffer: thread-safe deque ring buffer for N seconds of raw audio.
File write: atomic .tmp + os.replace() (mirrors vision-service JPEG write).
Rotation: delete oldest files beyond max_files count or max_days age.
"""

import os
import wave
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import numpy as np


# ---------------------------------------------------------------------------
# Pre-trigger ring buffer
# ---------------------------------------------------------------------------

class PreTriggerBuffer:
    """Stores the last N seconds of int16 audio blocks in a bounded deque.

    Callback thread calls push() every ~128 ms (2048 / 16000).
    Main thread calls drain() on anomaly to get pre-trigger audio.
    """

    def __init__(self, duration_s: float, sample_rate: int, block_size: int):
        self._sample_rate = sample_rate
        self._block_size = block_size
        self._max_blocks = int(duration_s * sample_rate / block_size) + 1
        self._buffer: deque = deque(maxlen=self._max_blocks)

    def push(self, block: np.ndarray) -> None:
        """Push one block of int16 samples.  Called from audio callback."""
        self._buffer.append(block.copy())

    def drain(self) -> np.ndarray:
        """Return all buffered blocks as a flat int16 array and clear."""
        if not self._buffer:
            return np.array([], dtype=np.int16)
        flat = np.concatenate(list(self._buffer), dtype=np.int16)
        return flat

    def clear(self) -> None:
        self._buffer.clear()

    @property
    def num_blocks(self) -> int:
        return len(self._buffer)


# ---------------------------------------------------------------------------
# File helpers
# ---------------------------------------------------------------------------

def ensure_dir(path: str) -> None:
    Path(path).mkdir(parents=True, exist_ok=True)


def build_wav_path(base: str, device_id: str, ts: datetime) -> str:
    """Canonical anomaly WAV path: {base}/anomalies/{device}_{ISO8601}.wav"""
    iso = ts.strftime("%Y%m%dT%H%M%S")
    return os.path.join(base, "anomalies", f"{device_id}_{iso}.wav")


def build_baseline_path(base: str, device_id: str, ts: datetime) -> str:
    """Baseline WAV path: {base}/baselines/{device}_{ISO8601}.wav"""
    iso = ts.strftime("%Y%m%dT%H%M%S")
    return os.path.join(base, "baselines", f"{device_id}_{iso}.wav")


def write_wav(file_path: str, audio_data: np.ndarray,
              sample_rate: int) -> bool:
    """Write int16 PCM as WAV.  Atomic via .tmp + os.replace().

    Returns True on success, False on error.
    """
    if audio_data.dtype != np.int16:
        audio_data = audio_data.astype(np.int16)

    tmp = file_path + ".tmp"
    try:
        ensure_dir(os.path.dirname(file_path))
        with wave.open(tmp, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)  # 16-bit
            wf.setframerate(sample_rate)
            wf.writeframes(audio_data.tobytes())
        os.replace(tmp, file_path)
        return True
    except OSError:
        # Disk full or permissions — clean up .tmp
        try:
            os.unlink(tmp)
        except OSError:
            pass
        return False


# ---------------------------------------------------------------------------
# Rotation helpers
# ---------------------------------------------------------------------------

def _list_wav_files(directory: str) -> list[str]:
    """Return absolute paths of .wav files in directory, oldest first."""
    try:
        files = sorted(
            [os.path.join(directory, f) for f in os.listdir(directory)
             if f.endswith(".wav")],
            key=os.path.getmtime,
        )
    except FileNotFoundError:
        return []
    return files


def rotate_expired_anomalies(base: str, max_files: int = 100,
                              max_days: int = 7) -> int:
    """Delete oldest anomaly WAVs exceeding max_files or max_days."""
    anomal_dir = os.path.join(base, "anomalies")
    files = _list_wav_files(anomal_dir)
    deleted = 0

    now = os.path.getmtime if hasattr(os.path, '_dummy')\
        else (lambda p: os.path.getmtime(p))

    for f in files[:]:  # iterate over copy
        try:
            age_days = (datetime.now().timestamp() - now(f)) / 86400.0
            if age_days > max_days:
                os.unlink(f)
                files.remove(f)
                deleted += 1
        except OSError:
            files.remove(f)

    # Then enforce max_files
    while len(files) > max_files:
        try:
            os.unlink(files[0])
            files.pop(0)
            deleted += 1
        except OSError:
            files.pop(0)

    return deleted


def rotate_baselines(base: str, max_files: int = 6) -> int:
    """Keep only the most recent `max_files` baseline WAVs."""
    bl_dir = os.path.join(base, "baselines")
    files = _list_wav_files(bl_dir)
    deleted = 0

    while len(files) > max_files:
        try:
            os.unlink(files[0])
            files.pop(0)
            deleted += 1
        except OSError:
            files.pop(0)

    return deleted
