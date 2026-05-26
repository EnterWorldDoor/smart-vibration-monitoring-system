"""Test deduplication logic for MQTT triggers.

Key behaviors:
  - First trigger always passes
  - Same (device, severity) within 5min window is suppressed
  - After window expiry, same (device, severity) passes again
  - Severity change (WARNING→CRITICAL or CRITICAL→WARNING) bypasses dedup
  - Different devices don't affect each other
"""

import time
from unittest.mock import MagicMock

import pytest

from src.config import MQTTConfig, DedupConfig
from src.mqtt.subscriber import MQTTSubscriber, TriggerEvent


@pytest.fixture
def subscriber():
    mqtt_cfg = MQTTConfig()
    dedup_cfg = DedupConfig(window_seconds=300, escalation_penetrate=True)
    sub = MQTTSubscriber(mqtt_cfg, dedup_cfg)
    sub.client = MagicMock()  # prevent real MQTT connection
    return sub


@pytest.fixture
def subscriber_no_escalation():
    mqtt_cfg = MQTTConfig()
    dedup_cfg = DedupConfig(window_seconds=300, escalation_penetrate=False)
    sub = MQTTSubscriber(mqtt_cfg, dedup_cfg)
    sub.client = MagicMock()
    return sub


class TestDedupFirstTrigger:
    def test_first_trigger_passes(self, subscriber):
        assert subscriber._check_dedup("factory1", "de01", "WARNING")

    def test_first_trigger_records_entry(self, subscriber):
        subscriber._check_dedup("factory1", "de01", "WARNING")
        assert ("factory1", "de01") in subscriber._dedup
        assert subscriber._dedup[("factory1", "de01")].severity == "WARNING"


class TestDedupSuppression:
    def test_same_device_severity_suppressed(self, subscriber):
        assert subscriber._check_dedup("factory1", "de01", "WARNING")
        assert not subscriber._check_dedup("factory1", "de01", "WARNING")

    def test_different_device_not_suppressed(self, subscriber):
        assert subscriber._check_dedup("factory1", "de01", "WARNING")
        assert subscriber._check_dedup("factory1", "de02", "WARNING")

    def test_different_site_not_suppressed(self, subscriber):
        assert subscriber._check_dedup("factory1", "de01", "WARNING")
        assert subscriber._check_dedup("factory2", "de01", "WARNING")


class TestDedupWindowExpiry:
    def test_window_expired_passes(self, subscriber):
        # Set window to 0.1 seconds for fast test
        subscriber.dedup_cfg.window_seconds = 0.1
        assert subscriber._check_dedup("factory1", "de01", "WARNING")
        time.sleep(0.15)
        assert subscriber._check_dedup("factory1", "de01", "WARNING")

    def test_cleanup_removes_expired(self, subscriber):
        subscriber.dedup_cfg.window_seconds = 0.05
        subscriber._check_dedup("factory1", "de01", "WARNING")
        time.sleep(0.2)
        subscriber._cleanup_expired()
        assert ("factory1", "de01") not in subscriber._dedup


class TestSeverityEscalation:
    def test_warning_to_critical_penetrates(self, subscriber):
        assert subscriber._check_dedup("factory1", "de01", "WARNING")
        # Immediate CRITICAL should penetrate
        assert subscriber._check_dedup("factory1", "de01", "CRITICAL")

    def test_critical_to_warning_penetrates(self, subscriber):
        assert subscriber._check_dedup("factory1", "de01", "CRITICAL")
        # Downgrade should still penetrate
        assert subscriber._check_dedup("factory1", "de01", "WARNING")

    def test_escalation_updates_severity(self, subscriber):
        subscriber._check_dedup("factory1", "de01", "WARNING")
        subscriber._check_dedup("factory1", "de01", "CRITICAL")
        assert subscriber._dedup[("factory1", "de01")].severity == "CRITICAL"


class TestDedupNoEscalation:
    def test_same_severity_still_suppressed(self, subscriber_no_escalation):
        assert subscriber_no_escalation._check_dedup("factory1", "de01", "WARNING")
        assert not subscriber_no_escalation._check_dedup("factory1", "de01", "WARNING")

    def test_escalation_to_critical_blocked(self, subscriber_no_escalation):
        assert subscriber_no_escalation._check_dedup("factory1", "de01", "WARNING")
        # Without escalation_penetrate, CRITICAL within window should also be suppressed
        assert not subscriber_no_escalation._check_dedup("factory1", "de01", "CRITICAL")


class TestMQTTMessageHandling:
    def test_normal_severity_ignored(self, subscriber):
        """Messages with NORMAL severity should not trigger."""
        triggered = []
        subscriber.set_trigger_callback(lambda e: triggered.append(e))

        msg = MagicMock()
        msg.topic = "EdgeVib/factory1/inference/de01/ai/report"
        msg.payload = b'{"severity": "NORMAL", "health_score": 90}'

        subscriber._on_message(None, None, msg)
        assert len(triggered) == 0

    def test_warning_severity_triggers(self, subscriber):
        triggered = []
        subscriber.set_trigger_callback(lambda e: triggered.append(e))

        msg = MagicMock()
        msg.topic = "EdgeVib/factory1/inference/de01/ai/report"
        msg.payload = b'{"severity": "WARNING", "health_score": 70}'

        subscriber._on_message(None, None, msg)
        assert len(triggered) == 1
        assert triggered[0].device_id == "de01"
        assert triggered[0].severity == "WARNING"

    def test_critical_severity_triggers(self, subscriber):
        triggered = []
        subscriber.set_trigger_callback(lambda e: triggered.append(e))

        msg = MagicMock()
        msg.topic = "EdgeVib/factory1/inference/de01/ai/report"
        msg.payload = b'{"severity": "CRITICAL", "health_score": 35}'

        subscriber._on_message(None, None, msg)
        assert len(triggered) == 1

    def test_invalid_json_ignored(self, subscriber):
        triggered = []
        subscriber.set_trigger_callback(lambda e: triggered.append(e))

        msg = MagicMock()
        msg.topic = "EdgeVib/factory1/inference/de01/ai/report"
        msg.payload = b"not json"

        subscriber._on_message(None, None, msg)
        assert len(triggered) == 0

    def test_short_topic_ignored(self, subscriber):
        triggered = []
        subscriber.set_trigger_callback(lambda e: triggered.append(e))

        msg = MagicMock()
        msg.topic = "EdgeVib/short"
        msg.payload = b'{"severity": "WARNING"}'

        subscriber._on_message(None, None, msg)
        assert len(triggered) == 0

    def test_duplicate_warning_suppressed_within_window(self, subscriber):
        subscriber.dedup_cfg.window_seconds = 5
        triggered = []
        subscriber.set_trigger_callback(lambda e: triggered.append(e))

        msg = MagicMock()
        msg.topic = "EdgeVib/factory1/inference/de01/ai/report"
        msg.payload = b'{"severity": "WARNING", "health_score": 70}'

        subscriber._on_message(None, None, msg)
        subscriber._on_message(None, None, msg)
        assert len(triggered) == 1  # Second suppressed
