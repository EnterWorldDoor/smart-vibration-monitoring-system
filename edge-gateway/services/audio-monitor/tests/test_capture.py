"""Test AudioCapture (mock sounddevice.InputStream)."""

import queue
from unittest.mock import MagicMock, patch

import numpy as np
import pytest

from src.audio.processor import AudioProcessor
from src.audio.capture import AudioCapture


class TestEnumerateDevices:
    @patch("src.audio.capture.sd")
    def test_enumerate_empty(self, mock_sd):
        mock_sd.query_devices.return_value = []
        devices = AudioCapture.enumerate_devices()
        assert devices == []

    @patch("src.audio.capture.sd")
    def test_enumerate_with_devices(self, mock_sd):
        mock_sd.query_devices.return_value = [
            {"name": "Mic 1", "max_input_channels": 2,
             "default_samplerate": 48000},
            {"name": "Speaker", "max_input_channels": 0,
             "default_samplerate": 48000},
            {"name": "Line In", "max_input_channels": 2,
             "default_samplerate": 44100},
        ]
        devices = AudioCapture.enumerate_devices()
        assert len(devices) == 2  # only input devices
        assert devices[0]["name"] == "Mic 1"
        assert devices[1]["name"] == "Line In"


class TestAudioCaptureLifecycle:
    def make_capture(self):
        proc = AudioProcessor()
        return AudioCapture(processor=proc)

    @patch("src.audio.capture.sd")
    def test_open_success(self, mock_sd):
        mock_stream = MagicMock()
        mock_sd.InputStream.return_value = mock_stream

        cap = self.make_capture()
        assert cap.open() is True
        mock_sd.InputStream.assert_called_once()
        mock_stream.start.assert_called_once()

    @patch("src.audio.capture.sd")
    def test_open_failure(self, mock_sd):
        import sounddevice as real_sd
        mock_sd.InputStream.side_effect = real_sd.PortAudioError("test error")

        cap = self.make_capture()
        assert cap.open() is False
        assert not cap.is_active()

    @patch("src.audio.capture.sd")
    def test_close(self, mock_sd):
        mock_stream = MagicMock()
        mock_sd.InputStream.return_value = mock_stream

        cap = self.make_capture()
        cap.open()
        cap.close()
        mock_stream.stop.assert_called_once()
        mock_stream.close.assert_called_once()
        assert not cap.is_active()

    @patch("src.audio.capture.sd")
    def test_open_already_open_closes_first(self, mock_sd):
        mock_stream1 = MagicMock()
        mock_stream2 = MagicMock()
        mock_sd.InputStream.side_effect = [mock_stream1, mock_stream2]

        cap = self.make_capture()
        cap.open()
        cap.open()
        mock_stream1.close.assert_called_once()


class TestAudioCallback:
    def make_capture(self):
        proc = AudioProcessor()
        return AudioCapture(processor=proc)

    @patch("src.audio.capture.sd")
    def test_callback_pushes_frame_to_queue(self, mock_sd):
        cap = self.make_capture()

        indata = np.random.randint(-1000, 1000, size=(2048, 1),
                                     dtype=np.int16)
        time_info = MagicMock()
        time_info.inputBufferAdcTime = 42.0

        cap._audio_callback(indata, 2048, time_info, None)

        # Frame should be in the queue
        try:
            frame = cap.get_queue().get(timeout=0.5)
        except queue.Empty:
            pytest.fail("No frame in queue after callback")

        assert frame is not None
        assert frame.timestamp == 42.0
        assert frame.raw_audio.shape == (2048,)
        assert frame.rms_energy is not None

    @patch("src.audio.capture.sd")
    def test_callback_overflow_doesnt_crash(self, mock_sd):
        cap = self.make_capture()

        indata = np.zeros((2048, 1), dtype=np.int16)
        time_info = MagicMock()
        time_info.inputBufferAdcTime = 0.0

        # Flood the queue
        for _ in range(300):
            cap._audio_callback(indata, 2048, time_info, None)

        # Should not have crashed; queue is capped
        assert cap.error_count == 0  # no status=error injected

    @patch("src.audio.capture.sd")
    def test_callback_status_overflow(self, mock_sd):
        cap = self.make_capture()

        status = MagicMock()
        status.input_overflow = True

        indata = np.zeros((2048, 1), dtype=np.int16)
        time_info = MagicMock()
        time_info.inputBufferAdcTime = 0.0

        cap._audio_callback(indata, 2048, time_info, status)
        assert cap.error_count == 1

    @patch("src.audio.capture.sd")
    def test_multichannel_flatten_to_mono(self, mock_sd):
        cap = self.make_capture()

        indata = np.zeros((2048, 2), dtype=np.int16)
        indata[:, 0] = 1000
        indata[:, 1] = 2000

        time_info = MagicMock()
        time_info.inputBufferAdcTime = 0.0

        cap._audio_callback(indata, 2048, time_info, None)

        frame = cap.get_queue().get(timeout=0.5)
        # Should be mono (2048,)
        assert frame.raw_audio.shape == (2048,)
        # Only channel 0 was taken
        assert frame.raw_audio[0] == 1000
