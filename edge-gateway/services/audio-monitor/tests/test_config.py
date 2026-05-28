"""Test YAML config loading and defaults."""

import os
import tempfile

import pytest

from src.config import Config


class TestConfigDefaults:
    def test_default_values(self):
        cfg = Config()
        assert cfg.audio.sample_rate == 16000
        assert cfg.audio.block_size == 2048
        assert cfg.audio.channels == 1
        assert cfg.audio.dtype == "int16"
        assert cfg.audio.device_index == -1
        assert cfg.alarm.sigma_warning == 3.0
        assert cfg.alarm.sigma_critical == 5.0
        assert cfg.alarm.learning_period_s == 60
        assert cfg.timescaledb.host == "localhost"
        assert cfg.timescaledb.port == 5432
        assert cfg.mqtt.broker == "localhost"
        assert cfg.mqtt.port == 1883

    def test_primary_ids_with_devices(self):
        cfg = Config()
        cfg.devices = [
            type("DeviceConfig", (), {
                "site_id": "factory1", "device_id": "audio01", "label": ""
            })()
        ]
        assert cfg.primary_site_id() == "factory1"
        assert cfg.primary_device_id() == "audio01"

    def test_primary_ids_empty(self):
        cfg = Config()
        assert cfg.primary_site_id() == "factory1"
        assert cfg.primary_device_id() == "audio01"


class TestConfigFromYaml:
    def test_full_yaml(self):
        yaml_content = """\
audio:
  sample_rate: 44100
  block_size: 4096
  device_index: 2
timescaledb:
  host: "db.local"
  port: 15432
mqtt:
  broker: "mqtt.local"
alarm:
  sigma_warning: 4.0
  learning_period_s: 120
logging:
  level: "debug"
"""
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".yaml", delete=False
        ) as f:
            f.write(yaml_content)
            path = f.name

        try:
            cfg = Config.from_yaml(path)
            assert cfg.audio.sample_rate == 44100
            assert cfg.audio.block_size == 4096
            assert cfg.audio.device_index == 2
            assert cfg.timescaledb.host == "db.local"
            assert cfg.timescaledb.port == 15432
            assert cfg.mqtt.broker == "mqtt.local"
            assert cfg.alarm.sigma_warning == 4.0
            assert cfg.alarm.learning_period_s == 120
        finally:
            os.unlink(path)

    def test_partial_yaml_keeps_defaults(self):
        yaml_content = "audio:\n  sample_rate: 8000\n"
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".yaml", delete=False
        ) as f:
            f.write(yaml_content)
            path = f.name

        try:
            cfg = Config.from_yaml(path)
            assert cfg.audio.sample_rate == 8000
            # defaults preserved
            assert cfg.alarm.sigma_warning == 3.0
            assert cfg.timescaledb.port == 5432
        finally:
            os.unlink(path)

    def test_empty_yaml(self):
        yaml_content = "{}"
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".yaml", delete=False
        ) as f:
            f.write(yaml_content)
            path = f.name

        try:
            cfg = Config.from_yaml(path)
            assert cfg.audio.sample_rate == 16000
        finally:
            os.unlink(path)
