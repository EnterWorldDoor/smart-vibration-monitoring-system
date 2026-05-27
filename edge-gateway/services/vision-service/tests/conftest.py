import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.config import DeviceConfig, CaptureConfig, MQTTConfig, TimescaleDBConfig, Config


@pytest.fixture
def sample_device():
    return DeviceConfig(
        site_id="factory1", device_type="motor",
        device_id="motor01", camera_index=0, label="Test Motor",
    )


@pytest.fixture
def sample_devices(sample_device):
    return [sample_device]


@pytest.fixture
def sample_capture_config(tmp_path):
    return CaptureConfig(storage_path=str(tmp_path / "vision"))


@pytest.fixture
def sample_mqtt_config():
    return MQTTConfig()


@pytest.fixture
def sample_db_config():
    return TimescaleDBConfig()


@pytest.fixture
def sample_config(sample_devices, sample_capture_config, sample_mqtt_config, sample_db_config):
    return Config(
        devices=sample_devices,
        capture=sample_capture_config,
        mqtt=sample_mqtt_config,
        timescaledb=sample_db_config,
    )


@pytest.fixture
def sample_jpeg_bytes():
    """A minimal valid JPEG — solid blue 1x1 pixel."""
    import numpy as np
    import cv2
    frame = np.zeros((1, 1, 3), dtype=np.uint8)
    frame[0, 0] = [255, 0, 0]
    _, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 95])
    return buf.tobytes()
