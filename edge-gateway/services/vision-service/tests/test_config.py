import tempfile

import yaml

from src.config import Config, DeviceConfig, CaptureConfig


def test_defaults():
    cfg = Config()
    assert cfg.devices == []
    assert cfg.capture.baseline_interval_s == 60


def test_from_yaml_multiple_devices():
    data = {
        "devices": [
            {"site_id": "site1", "device_id": "dev1", "camera_index": 0},
            {"site_id": "site2", "device_id": "dev2", "camera_index": 1},
        ]
    }
    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
        yaml.dump(data, f)
        f.flush()
        cfg = Config.from_yaml(f.name)

    assert len(cfg.devices) == 2
    assert cfg.devices[0].site_id == "site1"
    assert cfg.devices[1].camera_index == 1


def test_from_yaml_capture_defaults():
    data = {}
    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
        yaml.dump(data, f)
        f.flush()
        cfg = Config.from_yaml(f.name)

    assert cfg.capture.baseline_resolution == "640x480"
    assert cfg.capture.event_resolution == "1920x1080"
    assert cfg.capture.baseline_retention_days == 7
    assert cfg.capture.event_retention_days == 30


def test_capture_resolution_parsing():
    cc = CaptureConfig(baseline_resolution="320x240", event_resolution="1280x720")
    assert cc.baseline_width_height() == (320, 240)
    assert cc.event_width_height() == (1280, 720)


def test_primary_site_id():
    cfg = Config(devices=[DeviceConfig(site_id="factory_x")])
    assert cfg.primary_site_id() == "factory_x"


def test_primary_site_id_empty():
    cfg = Config()
    assert cfg.primary_site_id() == "unknown"


def test_device_config_defaults():
    d = DeviceConfig()
    assert d.site_id == "factory1"
    assert d.device_type == "motor"
    assert d.camera_index == 0
