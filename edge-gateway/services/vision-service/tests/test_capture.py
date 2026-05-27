from unittest.mock import patch, MagicMock

import numpy as np
import pytest
import cv2

from src.camera.capture import CameraCapture
from src.config import CaptureConfig


@pytest.fixture
def cap_cfg():
    return CaptureConfig()


@pytest.fixture
def mock_cv2():
    with patch("src.camera.capture.cv2") as m:
        # Default: camera opens successfully
        cap = MagicMock()
        cap.isOpened.return_value = True
        m.VideoCapture.return_value = cap
        yield m


def test_open_success(cap_cfg, mock_cv2):
    cam = CameraCapture(0, cap_cfg)
    assert cam.open() is True
    assert cam.is_open() is True
    mock_cv2.VideoCapture.assert_called_once_with(0)


def test_open_failure_is_opened_false(cap_cfg, mock_cv2):
    mock_cv2.VideoCapture.return_value.isOpened.return_value = False
    cam = CameraCapture(0, cap_cfg)
    assert cam.open() is False
    assert cam.is_open() is False


def test_read_frame_returns_frame(cap_cfg, mock_cv2):
    fake_frame = np.zeros((480, 640, 3), dtype=np.uint8)
    mock_cv2.VideoCapture.return_value.read.return_value = (True, fake_frame)

    cam = CameraCapture(0, cap_cfg)
    cam.open()
    result = cam.read_frame((640, 480))
    assert result is not None
    assert result.shape == (480, 640, 3)


def test_read_frame_resizes(cap_cfg, mock_cv2):
    fake_frame = np.zeros((1080, 1920, 3), dtype=np.uint8)
    mock_cv2.VideoCapture.return_value.read.return_value = (True, fake_frame)
    mock_cv2.resize.side_effect = lambda frame, size: np.zeros((size[1], size[0], 3), dtype=np.uint8)

    cam = CameraCapture(0, cap_cfg)
    cam.open()
    result = cam.read_frame((640, 480))
    assert result.shape == (480, 640, 3)


def test_read_frame_returns_none_on_failure(cap_cfg, mock_cv2):
    mock_cv2.VideoCapture.return_value.read.return_value = (False, None)

    cam = CameraCapture(0, cap_cfg)
    cam.open()
    result = cam.read_frame((640, 480))
    assert result is None


def test_read_frame_not_open(cap_cfg):
    cam = CameraCapture(0, cap_cfg)
    result = cam.read_frame((640, 480))
    assert result is None


def test_encode_jpeg(cap_cfg):
    cam = CameraCapture(0, cap_cfg)
    frame = np.zeros((480, 640, 3), dtype=np.uint8)
    frame[:] = [0, 128, 255]
    result = cam.encode_jpeg(frame, 75)
    assert isinstance(result, bytes)
    assert len(result) > 0


def test_encode_jpeg_valid_output(cap_cfg):
    """Encoded JPEG should be decodable."""
    cam = CameraCapture(0, cap_cfg)
    frame = np.zeros((480, 640, 3), dtype=np.uint8)
    frame[100:200, 100:200] = [0, 255, 0]
    jpeg_bytes = cam.encode_jpeg(frame, 95)
    decoded = cv2.imdecode(np.frombuffer(jpeg_bytes, np.uint8), cv2.IMREAD_COLOR)
    assert decoded is not None
    assert decoded.shape == (480, 640, 3)


def test_release(cap_cfg, mock_cv2):
    cam = CameraCapture(0, cap_cfg)
    cam.open()
    cam.release()
    mock_cv2.VideoCapture.return_value.release.assert_called_once()
    assert not cam.is_open()


def test_double_release_no_crash(cap_cfg, mock_cv2):
    cam = CameraCapture(0, cap_cfg)
    cam.open()
    cam.release()
    cam.release()
