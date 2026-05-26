"""Configuration loader for llm-analyzer."""

from dataclasses import dataclass, field

import yaml


@dataclass
class ModelConfig:
    path: str = "models/qwen2.5-1.5b-instruct-q4_k_m.gguf"
    context_length: int = 2048
    max_tokens: int = 512
    temperature: float = 0.7
    top_p: float = 0.9
    n_threads: int = 4


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
    client_id: str = "edgevib-llm-analyzer"
    qos: int = 1
    subscribe_topic: str = "EdgeVib/+/inference/+/ai/report"
    publish_topic: str = "EdgeVib/{site_id}/llm/{device_id}/report"


@dataclass
class DedupConfig:
    window_seconds: int = 300
    escalation_penetrate: bool = True


@dataclass
class ScheduleConfig:
    daily_summary_interval_h: int = 8
    health_report_interval_s: int = 30


@dataclass
class Config:
    model: ModelConfig = field(default_factory=ModelConfig)
    db: DBConfig = field(default_factory=DBConfig)
    mqtt: MQTTConfig = field(default_factory=MQTTConfig)
    dedup: DedupConfig = field(default_factory=DedupConfig)
    schedule: ScheduleConfig = field(default_factory=ScheduleConfig)
    prompts_dir: str = "prompts"
    site_id: str = "factory1"

    @classmethod
    def from_yaml(cls, path: str) -> "Config":
        with open(path) as f:
            data = yaml.safe_load(f)
        return cls._from_dict(data)

    @classmethod
    def _from_dict(cls, data: dict) -> "Config":
        return cls(
            model=ModelConfig(**data.get("model", {})),
            db=DBConfig(**data.get("db", {})),
            mqtt=MQTTConfig(**data.get("mqtt", {})),
            dedup=DedupConfig(**data.get("dedup", {})),
            schedule=ScheduleConfig(**data.get("schedule", {})),
            prompts_dir=data.get("prompts_dir", "prompts"),
            site_id=data.get("site_id", "factory1"),
        )
