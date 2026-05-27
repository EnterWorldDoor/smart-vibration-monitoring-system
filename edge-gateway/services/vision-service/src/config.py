"""Configuration loader for vision-service."""

from dataclasses import dataclass, field

import yaml


@dataclass
class DeviceConfig:
    """A single camera-to-motor mapping."""
    site_id: str = "factory1"
    device_type: str = "motor"
    device_id: str = "motor01"
    camera_index: int = 0
    label: str = ""


@dataclass
class CaptureConfig:
    baseline_interval_s: int = 60
    baseline_resolution: str = "640x480"
    baseline_quality: int = 75
    baseline_retention_days: int = 7
    event_resolution: str = "1920x1080"
    event_quality: int = 90
    event_retention_days: int = 30
    storage_path: str = "/opt/edge-gateway/data/vision"

    def baseline_width_height(self) -> tuple[int, int]:
        w, h = self.baseline_resolution.split("x")
        return int(w), int(h)

    def event_width_height(self) -> tuple[int, int]:
        w, h = self.event_resolution.split("x")
        return int(w), int(h)


@dataclass
class MQTTConfig:
    broker: str = "localhost"
    port: int = 1883
    client_id: str = "edgevib-vision-service"
    qos: int = 1
    subscribe_topic: str = "EdgeVib/+/inference/+/ai/report"
    publish_topic: str = "EdgeVib/{site_id}/vision/{device_id}/capture"


@dataclass
class TimescaleDBConfig:
    host: str = "localhost"
    port: int = 5432
    dbname: str = "edgevib_ts"
    user: str = "edgevib"
    password: str = "edgevib123"
    sslmode: str = "disable"


@dataclass
class Config:
    devices: list[DeviceConfig] = field(default_factory=list)
    capture: CaptureConfig = field(default_factory=CaptureConfig)
    mqtt: MQTTConfig = field(default_factory=MQTTConfig)
    timescaledb: TimescaleDBConfig = field(default_factory=TimescaleDBConfig)

    @classmethod
    def from_yaml(cls, path: str) -> "Config":
        with open(path) as f:
            data = yaml.safe_load(f)
        return cls._from_dict(data)

    @classmethod
    def _from_dict(cls, data: dict) -> "Config":
        devices = [DeviceConfig(**d) for d in data.get("devices", [])]
        capture = CaptureConfig(**data.get("capture", {}))
        mqtt = MQTTConfig(**data.get("mqtt", {}))
        timescaledb = TimescaleDBConfig(**data.get("timescaledb", {}))
        return cls(
            devices=devices, capture=capture,
            mqtt=mqtt, timescaledb=timescaledb,
        )

    def primary_site_id(self) -> str:
        return self.devices[0].site_id if self.devices else "unknown"
