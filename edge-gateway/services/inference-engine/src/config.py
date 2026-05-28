"""Configuration loader for inference-engine."""

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml


@dataclass
class ModelConfig:
    path: str = "models/autoencoder.onnx"
    metadata_path: str = "models/autoencoder_metadata.json"
    backend: str = "CPU"
    input_dim: int = 24

@dataclass
class DBConfig:
    host: str = "timescaledb"
    port: int = 5432
    user: str = "edgevib"
    password: str = "edgevib123"
    dbname: str = "edgevib_ts"
    sslmode: str = "disable"

@dataclass
class MQTTConfig:
    broker: str = "mosquitto"
    port: int = 1883
    client_id: str = "edgevib-inference-engine"
    qos: int = 1
    subscribe_topic: str = "EdgeVib/+/+/+/data/sensor"
    publish_topic: str = "EdgeVib/{site_id}/inference/{device_id}/ai/report"
    reload_topic: str = "EdgeVib/+/inference/+/model/reload"
    status_topic: str = "EdgeVib/{site_id}/inference/{device_id}/model/status"

@dataclass
class TriggerConfig:
    rms_threshold: float = 7.1          # mm/s, ISO 10816 Zone D
    bearing_fault_trigger: bool = True  # Immediate inference on bearing_fault detection
    low_confidence_trigger: float = 0.85  # Trigger if AI confidence below this
    lookback_minutes: int = 5           # Retrospective data window on trigger

@dataclass
class TrendConfig:
    window_size: int = 30               # Number of data points in sliding window
    rms_slope_warn: float = 0.05        # mm/s per point slope warning
    freq_drift_warn: float = 5.0        # Hz standard deviation warning
    temp_correlation_warn: float = 0.7  # Pearson r warning
    de_nde_ratio_warn: float = 0.3      # ±30% deviation from 1.0
    crest_factor_slope_warn: float = 0.02
    band_energy_shift_warn: float = 0.05  # 5%/day high-freq energy increase

@dataclass
class ScheduleConfig:
    inference_interval_s: int = 10
    health_report_interval_s: int = 30

@dataclass
class Config:
    model: ModelConfig = field(default_factory=ModelConfig)
    db: DBConfig = field(default_factory=DBConfig)
    mqtt: MQTTConfig = field(default_factory=MQTTConfig)
    trigger: TriggerConfig = field(default_factory=TriggerConfig)
    trend: TrendConfig = field(default_factory=TrendConfig)
    schedule: ScheduleConfig = field(default_factory=ScheduleConfig)
    site_id: str = "factory1"

    @classmethod
    def from_yaml(cls, path: str) -> "Config":
        with open(path) as f:
            data = yaml.safe_load(f)
        return cls._from_dict(data)

    @classmethod
    def _from_dict(cls, data: dict) -> "Config":
        model = ModelConfig(**data.get("model", {}))
        db = DBConfig(**data.get("db", {}))
        mqtt = MQTTConfig(**data.get("mqtt", {}))
        trigger = TriggerConfig(**data.get("trigger", {}))
        trend = TrendConfig(**data.get("trend", {}))
        schedule = ScheduleConfig(**data.get("schedule", {}))
        return cls(
            model=model, db=db, mqtt=mqtt,
            trigger=trigger, trend=trend, schedule=schedule,
            site_id=data.get("site_id", "factory1"),
        )
