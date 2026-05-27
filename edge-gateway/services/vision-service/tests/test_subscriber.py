import json

from src.config import MQTTConfig, DeviceConfig
from src.mqtt.subscriber import MQTTSubscriber, TriggerEvent


def _make_msg(topic: str, payload: dict) -> object:
    """Simulate paho MQTTMessage."""
    class FakeMsg:
        def __init__(self, t, p):
            self.topic = t
            self.payload = json.dumps(p).encode("utf-8")
    return FakeMsg(topic, payload)


def test_topic_parsing_and_device_match():
    cfg = MQTTConfig()
    devices = [
        DeviceConfig(site_id="factory1", device_id="motor01", camera_index=0),
        DeviceConfig(site_id="factory2", device_id="pump01", camera_index=1),
    ]
    sub = MQTTSubscriber(cfg, devices)

    calls = []
    sub.set_trigger_callback(lambda e, d: calls.append((e, d)))

    msg = _make_msg(
        "EdgeVib/factory1/inference/motor01/ai/report",
        {"severity": "WARNING", "trigger_reason": "rms_high"},
    )
    sub._on_message(None, None, msg)

    assert len(calls) == 1
    event, dev = calls[0]
    assert event.site_id == "factory1"
    assert event.device_id == "motor01"
    assert event.severity == "WARNING"
    assert dev.camera_index == 0


def test_non_matching_device_ignored():
    cfg = MQTTConfig()
    devices = [DeviceConfig(site_id="factory1", device_id="motor01")]
    sub = MQTTSubscriber(cfg, devices)

    calls = []
    sub.set_trigger_callback(lambda e, d: calls.append((e, d)))

    msg = _make_msg(
        "EdgeVib/factory1/inference/motor02/ai/report",
        {"severity": "WARNING"},
    )
    sub._on_message(None, None, msg)

    assert len(calls) == 0


def test_normal_severity_ignored():
    cfg = MQTTConfig()
    devices = [DeviceConfig(site_id="factory1", device_id="motor01")]
    sub = MQTTSubscriber(cfg, devices)

    calls = []
    sub.set_trigger_callback(lambda e, d: calls.append((e, d)))

    msg = _make_msg(
        "EdgeVib/factory1/inference/motor01/ai/report",
        {"severity": "NORMAL"},
    )
    sub._on_message(None, None, msg)

    assert len(calls) == 0


def test_missing_severity_ignored():
    cfg = MQTTConfig()
    devices = [DeviceConfig(site_id="factory1", device_id="motor01")]
    sub = MQTTSubscriber(cfg, devices)

    calls = []
    sub.set_trigger_callback(lambda e, d: calls.append((e, d)))

    msg = _make_msg(
        "EdgeVib/factory1/inference/motor01/ai/report",
        {"trigger_reason": "rms_high"},
    )
    sub._on_message(None, None, msg)

    assert len(calls) == 0


def test_invalid_json_silently_ignored():
    cfg = MQTTConfig()
    devices = [DeviceConfig(site_id="factory1", device_id="motor01")]
    sub = MQTTSubscriber(cfg, devices)

    calls = []
    sub.set_trigger_callback(lambda e, d: calls.append((e, d)))

    class BadMsg:
        topic = "EdgeVib/factory1/inference/motor01/ai/report"
        payload = b"not-json"

    sub._on_message(None, None, BadMsg())
    assert len(calls) == 0


def test_short_topic_ignored():
    cfg = MQTTConfig()
    devices = [DeviceConfig(site_id="factory1", device_id="motor01")]
    sub = MQTTSubscriber(cfg, devices)

    calls = []
    sub.set_trigger_callback(lambda e, d: calls.append((e, d)))

    class ShortMsg:
        topic = "EdgeVib/factory1/inference"
        payload = b'{"severity": "WARNING"}'

    sub._on_message(None, None, ShortMsg())
    assert len(calls) == 0


def test_critical_severity_triggers():
    cfg = MQTTConfig()
    devices = [DeviceConfig(site_id="factory1", device_id="motor01")]
    sub = MQTTSubscriber(cfg, devices)

    calls = []
    sub.set_trigger_callback(lambda e, d: calls.append((e, d)))

    msg = _make_msg(
        "EdgeVib/factory1/inference/motor01/ai/report",
        {"severity": "CRITICAL", "trigger_reason": "bearing_fault"},
    )
    sub._on_message(None, None, msg)

    assert len(calls) == 1
    assert calls[0][0].severity == "CRITICAL"


def test_device_map_lookup():
    cfg = MQTTConfig()
    devices = [
        DeviceConfig(site_id="f1", device_id="d1"),
        DeviceConfig(site_id="f2", device_id="d2"),
    ]
    sub = MQTTSubscriber(cfg, devices)

    assert sub._device_map[("f1", "d1")].site_id == "f1"
    assert sub._device_map[("f2", "d2")].device_id == "d2"
    assert ("f3", "d3") not in sub._device_map
