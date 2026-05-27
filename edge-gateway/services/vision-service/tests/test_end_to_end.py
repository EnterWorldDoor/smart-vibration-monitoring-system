import os
import time
from datetime import datetime, timezone
from unittest.mock import patch, MagicMock

import numpy as np
import cv2

from src.config import Config
from src.mqtt.subscriber import TriggerEvent
from src.__main__ import VisionService


def _make_fake_frame(w=1920, h=1080):
    frame = np.zeros((h, w, 3), dtype=np.uint8)
    frame[100:200, 100:200] = [0, 255, 0]
    return frame


@patch("src.__main__.MQTTPublisher")
@patch("src.__main__.MQTTSubscriber")
@patch("src.__main__.HealthReporter")
@patch("src.__main__.VisonDBClient")
def test_end_to_end_baseline_capture(
    mock_db_cls, mock_health_cls, mock_sub_cls, mock_pub_cls, tmp_path, sample_config,
):
    """Full pipeline: capture a baseline frame and verify file is written."""
    sample_config.capture.storage_path = str(tmp_path / "vision")

    mock_db = mock_db_cls.return_value
    mock_sub = mock_sub_cls.return_value
    mock_pub = mock_pub_cls.return_value
    mock_health = mock_health_cls.return_value
    mock_health.total_baselines = 0
    mock_health.total_events = 0
    mock_health.errors = 0

    service = VisionService.__new__(VisionService)
    service.cfg = sample_config
    service._running = False
    service._event_queue = type("Queue", (), {"empty": lambda: True})()
    service._db = mock_db
    service._mqtt_pub = mock_pub
    service._mqtt_sub = mock_sub
    service._mqtt_pub = mock_pub
    service._health = mock_health

    fake_cam = MagicMock()
    fake_frame = _make_fake_frame(640, 480)
    fake_cam.read_frame.return_value = fake_frame
    fake_cam.encode_jpeg.return_value = cv2.imencode(
        ".jpg", fake_frame, [cv2.IMWRITE_JPEG_QUALITY, 75])[1].tobytes()
    fake_cam.is_open.return_value = True
    service._cameras = {0: fake_cam}

    service._capture_all_baselines()

    assert mock_health.total_baselines == 1
    mock_db.insert_capture.assert_called_once()
    mock_pub.publish_capture.assert_called_once()

    call_args = mock_db.insert_capture.call_args[1]
    assert call_args["capture_type"] == "baseline"
    assert call_args["trigger_src"] == "timer"
    assert os.path.isfile(call_args["file_path"])


@patch("src.__main__.MQTTPublisher")
@patch("src.__main__.MQTTSubscriber")
@patch("src.__main__.HealthReporter")
@patch("src.__main__.VisonDBClient")
def test_end_to_end_event_capture(
    mock_db_cls, mock_health_cls, mock_sub_cls, mock_pub_cls, tmp_path, sample_config,
):
    """Event capture triggered by MQTT WARNING."""
    sample_config.capture.storage_path = str(tmp_path / "vision")

    mock_db = mock_db_cls.return_value
    mock_sub = mock_sub_cls.return_value
    mock_pub = mock_pub_cls.return_value
    mock_health = mock_health_cls.return_value
    mock_health.total_baselines = 0
    mock_health.total_events = 0
    mock_health.errors = 0

    service = VisionService.__new__(VisionService)
    service.cfg = sample_config
    service._running = False
    service._event_queue = type("Queue", (), {"empty": lambda: True})()
    service._db = mock_db
    service._mqtt_pub = mock_pub
    service._mqtt_sub = mock_sub
    service._health = mock_health

    fake_cam = MagicMock()
    fake_frame = _make_fake_frame(1920, 1080)
    fake_cam.read_frame.return_value = fake_frame
    fake_cam.encode_jpeg.return_value = cv2.imencode(
        ".jpg", fake_frame, [cv2.IMWRITE_JPEG_QUALITY, 90])[1].tobytes()
    fake_cam.is_open.return_value = True
    service._cameras = {0: fake_cam}

    from src.config import DeviceConfig
    dev = DeviceConfig(site_id="factory1", device_id="motor01", camera_index=0)
    event = TriggerEvent(
        site_id="factory1", device_id="motor01",
        severity="CRITICAL", trigger_reason="bearing_fault",
    )

    service._capture_event(event, dev)

    assert mock_health.total_events == 1
    mock_db.insert_capture.assert_called_once()
    call_args = mock_db.insert_capture.call_args[1]
    assert call_args["capture_type"] == "event"
    assert call_args["trigger_src"] == "mqtt_inference"
    assert "CRITICAL" in call_args["file_path"]
    assert os.path.isfile(call_args["file_path"])


@patch("src.__main__.MQTTPublisher")
@patch("src.__main__.MQTTSubscriber")
@patch("src.__main__.HealthReporter")
@patch("src.__main__.VisonDBClient")
def test_camera_reconnection(
    mock_db_cls, mock_health_cls, mock_sub_cls, mock_pub_cls, sample_config,
):
    """_get_or_open_camera should re-open a camera that was disconnected."""
    mock_db = mock_db_cls.return_value
    mock_sub = mock_sub_cls.return_value
    mock_pub = mock_pub_cls.return_value
    mock_health = mock_health_cls.return_value
    mock_health.total_baselines = 0
    mock_health.total_events = 0
    mock_health.errors = 0

    service = VisionService.__new__(VisionService)
    service.cfg = sample_config
    service._cameras = {}
    service._db = mock_db
    service._mqtt_pub = mock_pub
    service._mqtt_sub = mock_sub
    service._health = mock_health

    from src.config import DeviceConfig
    dev = DeviceConfig(site_id="factory1", device_id="motor01", camera_index=0)

    with patch("src.__main__.CameraCapture") as mock_cam_cls:
        mock_cam = mock_cam_cls.return_value
        mock_cam.open.return_value = True
        mock_cam.is_open.return_value = True

        cam = service._get_or_open_camera(dev)
        assert cam is not None
        assert service._cameras[0] is mock_cam

    with patch("src.__main__.CameraCapture") as mock_cam_cls2:
        bad_cam = MagicMock()
        bad_cam.open.return_value = False
        mock_cam_cls2.return_value = bad_cam

        service._cameras = {}
        cam = service._get_or_open_camera(dev)
        assert cam is None
        assert 0 not in service._cameras
