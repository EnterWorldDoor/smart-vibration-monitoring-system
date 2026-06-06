"""End-to-end tests for edge-router service.

Requires:
- Mosquitto MQTT broker at localhost:1883 (or MOSQUITTO_HOST env)
- edge-router Docker container running

Usage:
    docker compose -f docker/docker-compose.yml up -d edge-router
    pytest tests/edge-router/test_e2e.py -v
"""

import json
import time
import os
import pytest
import paho.mqtt.client as mqtt


MOSQUITTO_HOST = os.environ.get("MOSQUITTO_HOST", "localhost")
MOSQUITTO_PORT = int(os.environ.get("MOSQUITTO_PORT", "1883"))
ROUTED_TOPIC = "EdgeVib/+/router/+/alert"
TIMEOUT = 10  # seconds to wait for routed alerts


def mqtt_connect(client_id: str) -> mqtt.Client:
    """Create and connect a test MQTT client."""
    client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
    client.connect(MOSQUITTO_HOST, MOSQUITTO_PORT, 60)
    client.loop_start()
    return client


def wait_for_message(sub_client: mqtt.Client, timeout: float = TIMEOUT) -> dict | None:
    """Wait for a message on the subscribed topic, return parsed JSON payload."""
    received = []

    def on_msg(_client, _userdata, msg):
        try:
            received.append({
                "topic": msg.topic,
                "payload": json.loads(msg.payload),
            })
        except json.JSONDecodeError:
            received.append({"topic": msg.topic, "payload": msg.payload})

    sub_client.on_message = on_msg
    sub_client.subscribe(ROUTED_TOPIC, qos=1)

    deadline = time.time() + timeout
    while not received and time.time() < deadline:
        time.sleep(0.1)

    sub_client.unsubscribe(ROUTED_TOPIC)
    return received[0] if received else None


@pytest.fixture(scope="module")
def pub_client():
    """MQTT publisher client shared across tests."""
    client = mqtt_connect("test-edge-router-pub")
    yield client
    client.loop_stop()
    client.disconnect()


@pytest.fixture
def sub_client():
    """MQTT subscriber client — fresh per test to avoid message bleed."""
    client = mqtt_connect("test-edge-router-sub")
    yield client
    client.loop_stop()
    client.disconnect()


class TestESP32FaultRouting:
    """Test ESP32 sensor messages with fault classifications."""

    def test_bearing_fault_routes_to_target(self, pub_client, sub_client):
        """ESP32 bearing_fault should be routed to target site."""
        payload = json.dumps({
            "timestamp_ms": 1234567890,
            "data": {
                "ai": {
                    "class_name": "bearing_fault",
                    "confidence": 0.92,
                },
                "vibration": {
                    "overall_rms": 8.7,
                },
                "environment": {
                    "temperature_c": 72.0,
                },
            },
        })
        pub_client.publish(
            "EdgeVib/factory1/motor/motor01/data/sensor",
            payload, qos=1,
        )

        result = wait_for_message(sub_client)
        assert result is not None, f"No alert received on {ROUTED_TOPIC} within {TIMEOUT}s"

        p = result["payload"]
        assert p["source_site"] == "factory1"
        assert p["source_device"] == "motor01"
        assert p["alert_source"] == "esp32"
        assert p["ai_class"] == "bearing_fault"
        assert p["confidence"] == 0.92
        assert p["rms_current"] == 8.7
        assert p["temperature_c"] == 72.0
        assert "context_snapshot" in p

    def test_normal_sensor_not_routed(self, pub_client, sub_client):
        """Normal classification should NOT be routed."""
        payload = json.dumps({
            "timestamp_ms": 1234567891,
            "data": {
                "ai": {
                    "class_name": "normal",
                    "confidence": 0.99,
                },
                "vibration": {
                    "overall_rms": 2.1,
                },
                "environment": {
                    "temperature_c": 45.0,
                },
            },
        })
        pub_client.publish(
            "EdgeVib/factory1/motor/motor02/data/sensor",
            payload, qos=1,
        )

        result = wait_for_message(sub_client, timeout=3)
        assert result is None, f"Normal message should not be routed, got: {result}"

    def test_empty_ai_class_not_routed(self, pub_client, sub_client):
        """Empty ai.class_name should NOT be routed."""
        payload = json.dumps({
            "timestamp_ms": 1234567892,
            "data": {
                "vibration": {
                    "overall_rms": 2.5,
                },
            },
        })
        pub_client.publish(
            "EdgeVib/factory1/motor/motor01/data/sensor",
            payload, qos=1,
        )

        result = wait_for_message(sub_client, timeout=3)
        assert result is None, f"Empty class should not be routed, got: {result}"


class TestInferenceReportRouting:
    """Test inference-engine report messages."""

    def test_inference_anomaly_routes(self, pub_client, sub_client):
        """Inference WARNING report should be routed to target."""
        payload = json.dumps({
            "anomaly_detected": True,
            "health_score": 45.0,
            "severity": "WARNING",
            "anomaly_score": 0.88,
            "summary": "Motor motor01: health=45; anomaly detected",
            "warnings": ["rms_slope rising sharply"],
            "timestamp_utc": "2026-06-06T14:30:00Z",
        })
        pub_client.publish(
            "EdgeVib/factory2/inference/motor01/ai/report",
            payload, qos=1,
        )

        result = wait_for_message(sub_client)
        assert result is not None, f"No alert received on {ROUTED_TOPIC} within {TIMEOUT}s"

        p = result["payload"]
        assert p["source_site"] == "factory2"
        assert p["source_device"] == "motor01"
        assert p["alert_source"] == "inference"
        assert p["alert_level"] == "warning"
        assert p["severity"] == "WARNING"
        assert p["health_score"] == 45.0


class TestDedup:
    """Test deduplication behavior."""

    def test_duplicate_suppressed(self, pub_client, sub_client):
        """Duplicate alert within 30s window should be suppressed."""
        payload = json.dumps({
            "timestamp_ms": 1234567900,
            "data": {
                "ai": {
                    "class_name": "misalignment",
                    "confidence": 0.85,
                },
                "vibration": {
                    "overall_rms": 5.5,
                },
                "environment": {
                    "temperature_c": 55.0,
                },
            },
        })

        # First send — should route
        pub_client.publish(
            "EdgeVib/factory1/motor/motor01/data/sensor",
            payload, qos=1,
        )
        first = wait_for_message(sub_client)
        assert first is not None, "First alert should be routed"

        # Second send (same source_site, source_device, alert_source) — should NOT route
        pub_client.publish(
            "EdgeVib/factory1/motor/motor01/data/sensor",
            payload, qos=1,
        )
        second = wait_for_message(sub_client, timeout=3)
        assert second is None, f"Duplicate alert should be deduped, got: {second}"

    def test_different_alert_source_not_deduped(self, pub_client, sub_client):
        """ESP32 and inference alerts for same device should both route."""
        # NOTE: This test depends on dedup cache state from previous tests.
        # We use a different device_id to avoid collision.
        esp32_payload = json.dumps({
            "timestamp_ms": 1234567800,
            "data": {
                "ai": {
                    "class_name": "bearing_fault",
                    "confidence": 0.91,
                },
                "vibration": {
                    "overall_rms": 7.2,
                },
                "environment": {
                    "temperature_c": 60.0,
                },
            },
        })
        pub_client.publish(
            "EdgeVib/factory2/motor/motor02/data/sensor",
            esp32_payload, qos=1,
        )

        inf_payload = json.dumps({
            "anomaly_detected": True,
            "health_score": 30.0,
            "severity": "CRITICAL",
            "anomaly_score": 0.95,
            "summary": "Critical anomaly",
            "warnings": ["bearing_fault"],
            "timestamp_utc": "2026-06-06T14:31:00Z",
        })
        pub_client.publish(
            "EdgeVib/factory2/inference/motor02/ai/report",
            inf_payload, qos=1,
        )

        # Both should arrive (different alert_source: esp32 vs inference)
        results = []
        deadline = time.time() + TIMEOUT
        sub_client.subscribe(ROUTED_TOPIC, qos=1)

        def on_msg(_client, _userdata, msg):
            try:
                results.append({
                    "topic": msg.topic,
                    "payload": json.loads(msg.payload),
                })
            except json.JSONDecodeError:
                pass

        sub_client.on_message = on_msg
        while len(results) < 2 and time.time() < deadline:
            time.sleep(0.1)

        sub_client.unsubscribe(ROUTED_TOPIC)

        sources = [r["payload"]["alert_source"] for r in results]
        assert "esp32" in sources, f"ESP32 alert should route, got sources: {sources}"
        assert "inference" in sources, f"Inference alert should route, got sources: {sources}"


class TestHealthReport:
    """Test health reporting."""

    def test_health_report_published(self, pub_client):
        """Health report should be valid JSON with required fields."""
        received = []
        client = mqtt_connect("test-edge-router-health")

        def on_msg(_client, _userdata, msg):
            try:
                received.append(json.loads(msg.payload))
            except json.JSONDecodeError:
                pass

        client.on_message = on_msg
        client.subscribe("EdgeVib/+/router/orangepi/status/health", qos=1)

        deadline = time.time() + 35  # health reports every 30s
        while not received and time.time() < deadline:
            time.sleep(0.5)

        client.loop_stop()
        client.disconnect()

        assert received, "No health report received within 35s"

        report = received[0]
        assert report["service"] == "edge-router"
        assert report["version"] == "1.0.0"
        assert "msg_received" in report
        assert "alerts_routed" in report
        assert "alerts_deduped" in report
        assert "uptime_seconds" in report


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
