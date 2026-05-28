"""sounddevice InputStream wrapper with callback-to-queue bridge.

Manages the InputStream lifecycle.  The portaudio C-thread callback does
FFT + feature extraction + queue push (fast, <1 ms).  The Python main
thread consumes frames from the queue for baseline/anomaly/DB/MQTT ops.

CRITICAL: InputStream must be created and destroyed in the same thread.
AudioCapture is instantiated and managed entirely from the main thread.
"""

from dataclasses import dataclass
from queue import Queue
from typing import Optional

import numpy as np
import structlog

try:
    import sounddevice as sd
    HAS_SOUNDDEVICE = True
except ImportError:
    sd = None                # type: ignore
    HAS_SOUNDDEVICE = False

from src.audio.processor import AudioProcessor, AudioFrame

logger = structlog.get_logger(__name__)


# Re-export for convenience
__all__ = ["AudioCapture", "AudioFrame"]


class AudioCapture:
    """Manages a sounddevice InputStream with callback processing."""

    def __init__(
        self,
        processor: AudioProcessor,
        sample_rate: int = 16000,
        block_size: int = 2048,
        channels: int = 1,
        dtype: str = "int16",
        device_index: int = -1,
        queue_size: int = 256,
    ):
        self._processor = processor
        self._sample_rate = sample_rate
        self._block_size = block_size
        self._channels = channels
        self._dtype = dtype
        self._device_index = device_index
        self._frame_queue: Queue = Queue(maxsize=queue_size)

        self._stream: Optional[sd.InputStream] = None
        self._error_count: int = 0

    # --- Public API ----------------------------------------------------------

    @property
    def error_count(self) -> int:
        return self._error_count

    @staticmethod
    def enumerate_devices() -> list[dict]:
        """List available audio input devices."""
        if not HAS_SOUNDDEVICE:
            return []
        devices = sd.query_devices()
        result = []
        for i, d in enumerate(devices):
            if d["max_input_channels"] > 0:
                result.append({
                    "index": i,
                    "name": d["name"],
                    "max_input_channels": d["max_input_channels"],
                    "default_samplerate": int(d["default_samplerate"]),
                })
        return result

    def open(self) -> bool:
        """Create and start the InputStream.  Returns True on success."""
        if not HAS_SOUNDDEVICE:
            logger.warning("sounddevice not installed, cannot open audio")
            return False

        if self._stream is not None:
            self.close()

        try:
            self._stream = sd.InputStream(
                samplerate=self._sample_rate,
                blocksize=self._block_size,
                device=self._device_index,
                channels=self._channels,
                dtype=self._dtype,
                callback=self._audio_callback,
            )
            self._stream.start()
            logger.info("audio stream opened",
                        device_index=self._device_index,
                        sample_rate=self._sample_rate,
                        block_size=self._block_size)
            return True
        except (sd.PortAudioError, OSError) as exc:
            logger.warning("audio stream open failed", error=str(exc))
            self._stream = None
            return False

    def close(self) -> None:
        """Stop and close the InputStream."""
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            except (sd.PortAudioError, OSError):
                pass
            self._stream = None
            logger.info("audio stream closed")

    def is_active(self) -> bool:
        return self._stream is not None and self._stream.active

    def get_queue(self) -> Queue:
        return self._frame_queue

    # --- Portaudio callback (C thread) --------------------------------------

    def _audio_callback(self, indata: np.ndarray, frames: int,
                         time_info, status) -> None:
        """Called by portaudio internal thread every ~128 ms.

        MUST not block, allocate heap memory via Python C-API, or raise.
        numpy operations via BLAS are OK; they don't touch Python allocators.
        """
        if status:
            self._error_count += 1
            if status.input_overflow:
                logger.debug("audio input overflow")

        # Flatten to mono if needed
        mono = indata[:, 0].copy() if indata.ndim > 1 else indata.copy()

        # FFT + features (pure numpy, no I/O)
        frame = self._processor.process_block(mono)
        frame.timestamp = time_info.inputBufferAdcTime

        # Push to main thread queue (non-blocking)
        try:
            self._frame_queue.put_nowait(frame)
        except Exception:
            pass  # main thread falling behind, drop frame silently
