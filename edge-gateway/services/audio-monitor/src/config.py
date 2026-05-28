"""Configuration dataclasses for audio-monitor service.

Mirrors vision-service src/config.py pattern: dataclass per YAML section,
flat Config aggregator, YAML-safe classmethod loader.
"""

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml


@dataclass
class AudioConfig:
    device_index: int = -1
    sample_rate: int = 16000
    block_size: int = 2048
    channels: int = 1
    dtype: str = "int16"
    overlap_ratio: float = 0.5
    storage_path: str = "/var/lib/edgevib/audio"


@dataclass
class TimescaleDBConfig:
    host: str = "localhost"
    port: int = 5432
    dbname: str = "edgevib_ts"
    user: str = "edgevib"
    password: str = "edgevib123"
    sslmode: str = "disable"


@dataclass
class MQTTConfig:
    broker: str = "localhost"
    port: int = 1883
    client_id: str = "edgevib-audio-monitor"
    qos: int = 1
    subscribe_topic: str = "EdgeVib/+/inference/+/ai/report"
    publish_topic: str = "EdgeVib/{site_id}/audio/{device_id}/alert"


@dataclass
class AlarmConfig:
    sigma_warning: float = 3.0
    sigma_critical: float = 5.0
    baseline_window_s: int = 60
    anomaly_sustain_s: int = 30
    pre_trigger_s: int = 3
    post_trigger_s: int = 2
    learning_period_s: int = 60


@dataclass
class LoggingConfig:
    level: str = "info"


@dataclass
class DeviceConfig:
    site_id: str = "factory1"
    device_id: str = "audio01"
    label: str = ""


@dataclass
class Config:
    devices: list[DeviceConfig] = field(default_factory=list)
    audio: AudioConfig = field(default_factory=AudioConfig)
    timescaledb: TimescaleDBConfig = field(default_factory=TimescaleDBConfig)
    mqtt: MQTTConfig = field(default_factory=MQTTConfig)
    alarm: AlarmConfig = field(default_factory=AlarmConfig)
    logging: LoggingConfig = field(default_factory=LoggingConfig)

    @classmethod
    def from_yaml(cls, path: str) -> "Config":
        with open(path) as f:
            data = yaml.safe_load(f) or {}
        return cls._from_dict(data)

    @classmethod
    def _from_dict(cls, data: dict) -> "Config":
        devices = [DeviceConfig(**d) for d in data.get("devices", [])]
        audio = AudioConfig(**data.get("audio", {}))
        tsdb = TimescaleDBConfig(**data.get("timescaledb", {}))
        mqtt = MQTTConfig(**data.get("mqtt", {}))
        alarm = AlarmConfig(**data.get("alarm", {}))
        logging = LoggingConfig(**data.get("logging", {}))
        return cls(
            devices=devices, audio=audio, timescaledb=tsdb,
            mqtt=mqtt, alarm=alarm, logging=logging,
        )

    def primary_site_id(self) -> str:
        if self.devices:
            return self.devices[0].site_id
        return "factory1"

    def primary_device_id(self) -> str:
        if self.devices:
            return self.devices[0].device_id
        return "audio01"
