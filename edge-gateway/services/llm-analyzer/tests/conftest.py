"""Shared test fixtures for llm-analyzer tests."""

import sys
import time
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

# Add services/llm-analyzer/ to path for src.* imports
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


@pytest.fixture
def mock_db_client():
    """Mock DBClient with canned responses."""
    db = MagicMock()
    db.query_recent_ai_reports.return_value = [
        {
            "time": "2026-05-26T10:00:00Z",
            "report_type": "motor_health",
            "device_id": "de01",
            "severity": "CRITICAL",
            "health_score": 35.0,
            "anomaly_score": 0.52,
            "details": {
                "rms_slope": 0.12,
                "freq_drift_std": 8.3,
                "crest_factor_slope": 0.035,
                "trend_warnings": [
                    "RMS rising: 0.120 mm/s per point",
                    "Freq drift: σ=8.3 Hz",
                ],
                "anomaly_detected": True,
            },
        }
    ]
    db.query_recent_vibration.return_value = [
        {"time": f"2026-05-26T09:50:{i:02d}Z",
         "overall_rms": 8.0 + i * 0.1,
         "peak_frequency_hz": 245.0 + i * 0.5,
         "rms_x": 6.0, "rms_y": 5.5, "rms_z": 4.0}
        for i in range(10)
    ]
    db.query_ai_reports_in_window.return_value = [
        {
            "time": "2026-05-26T08:00:00Z",
            "report_type": "motor_health",
            "device_id": "de01",
            "severity": "CRITICAL",
            "health_score": 35.0,
            "anomaly_score": 0.52,
            "details": {"anomaly_detected": True, "trend_warnings": ["RMS rising"]},
        },
        {
            "time": "2026-05-26T07:00:00Z",
            "report_type": "motor_health",
            "device_id": "nde01",
            "severity": "WARNING",
            "health_score": 72.0,
            "anomaly_score": 0.20,
            "details": {"anomaly_detected": False},
        },
        {
            "time": "2026-05-26T06:00:00Z",
            "report_type": "motor_health",
            "device_id": "pump01",
            "severity": "NORMAL",
            "health_score": 95.0,
            "anomaly_score": 0.05,
            "details": {"anomaly_detected": False},
        },
    ]
    return db


@pytest.fixture
def mock_llm_engine():
    """Mock LLMEngine with canned generation output."""
    engine = MagicMock()
    engine.model_name = "qwen2.5-1.5b-instruct"
    engine.model_version = "q4_k_m"
    engine.is_loaded = True
    engine.chat.return_value = {
        "text": (
            "【标题】motor de01 驱动端轴承故障恶化\n"
            "【当前状态】\n设备运行在危险区间，RMS达到8.5 mm/s。\n"
            "【异常分析】\n1. 振动总量超标\n2. 轴承故障特征\n3. 早期缺陷信号\n"
            "【维护建议】\n1. 立即安排停机检查\n2. 重点检查驱动端轴承\n"
            "3. 检查联轴器对中"
        ),
        "tokens_used": 245,
        "generation_time_ms": 15234.5,
    }
    return engine


@pytest.fixture
def mock_mqtt_publisher():
    """Mock MQTTPublisher."""
    pub = MagicMock()
    pub.publish_report.return_value = True
    return pub


@pytest.fixture
def mock_health():
    """Mock HealthReporter."""
    from src.health import HealthReporter
    from src.config import MQTTConfig

    health = HealthReporter(MQTTConfig(), "factory1")
    health.client = MagicMock()
    return health


@pytest.fixture
def sample_trigger_event():
    """Sample TriggerEvent for alert report building."""
    from src.mqtt.subscriber import TriggerEvent

    return TriggerEvent(
        topic="EdgeVib/factory1/inference/de01/ai/report",
        site_id="factory1",
        device_id="de01",
        severity="CRITICAL",
        trigger_reason="ai_bearing_fault",
        payload={"health_score": 35.0, "anomaly_score": 0.52},
        timestamp=time.monotonic(),
    )
