"""MQTT subscriber with trigger detection (AI anomaly / RMS threshold)."""

import json
import structlog
from dataclasses import dataclass, field
from typing import Callable, Optional

import paho.mqtt.client as mqtt

from src.config import MQTTConfig, TriggerConfig

logger = structlog.get_logger(__name__)


@dataclass
class TriggerEvent:
    topic: str
    site_id: str
    device_type: str
    device_id: str
    trigger_reason: str  # "ai_bearing_fault" | "low_confidence" | "rms_high"
    payload: dict = field(default_factory=dict)


class MQTTSubscriber:
    def __init__(self, mqtt_cfg: MQTTConfig, trigger_cfg: TriggerConfig):
        self.mqtt_cfg = mqtt_cfg
        self.trigger_cfg = trigger_cfg
        self.client = mqtt.Client(client_id=mqtt_cfg.client_id, protocol=mqtt.MQTTv311)
        self._trigger_callback: Optional[Callable[[TriggerEvent], None]] = None
        self._reload_callback: Optional[Callable[[str, dict], None]] = None
        self._connected = False

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)

    def set_trigger_callback(self, cb: Callable[[TriggerEvent], None]):
        self._trigger_callback = cb

    def set_reload_callback(self, cb: Callable[[str, dict], None]):
        self._reload_callback = cb

    def connect(self):
        self.client.connect(self.mqtt_cfg.broker, self.mqtt_cfg.port, 60)
        self.client.loop_start()
        logger.info("mqtt subscriber connecting", broker=self.mqtt_cfg.broker)

    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()

    def is_connected(self) -> bool:
        return self._connected

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._connected = True
            topic = self.mqtt_cfg.subscribe_topic
            client.subscribe(topic, qos=self.mqtt_cfg.qos)
            logger.info("mqtt subscribed", topic=topic)
            reload_topic = self.mqtt_cfg.reload_topic
            client.subscribe(reload_topic, qos=self.mqtt_cfg.qos)
            logger.info("mqtt subscribed reload", topic=reload_topic)
        else:
            logger.error("mqtt connect failed", rc=rc)

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        if rc != 0:
            logger.warning("mqtt disconnected unexpectedly", rc=rc)

    def _on_message(self, client, userdata, msg):
        # Reload topic: fast path (no trigger parsing needed)
        if "/model/reload" in msg.topic:
            if self._reload_callback:
                try:
                    payload = json.loads(msg.payload)
                    self._reload_callback(msg.topic, payload)
                except json.JSONDecodeError:
                    logger.warning("reload: invalid JSON payload")
            return

        try:
            payload = json.loads(msg.payload)
        except json.JSONDecodeError:
            return

        topic_parts = msg.topic.split("/")
        if len(topic_parts) < 5:
            return

        site_id = topic_parts[1]
        device_type = topic_parts[2]
        device_id = topic_parts[3]

        trigger = self._check_trigger(payload)
        if trigger and self._trigger_callback:
            trigger.site_id = site_id
            trigger.device_type = device_type
            trigger.device_id = device_id
            trigger.topic = msg.topic
            trigger.payload = payload
            self._trigger_callback(trigger)

    def _check_trigger(self, payload: dict) -> Optional[str]:
        ai = payload.get("data", {}).get("ai", {})
        class_name = ai.get("class_name", "").lower()
        confidence = ai.get("confidence", 1.0)
        cascade_source = ai.get("cascade_source", "")

        vibration = payload.get("data", {}).get("vibration", {})
        overall_rms = vibration.get("overall_rms", 0.0)

        # Bearing fault trigger
        if (class_name == "bearing_fault" and
                confidence >= self.trigger_cfg.confidence_min):
            return "ai_bearing_fault"

        # Low confidence cascade trigger (primary model uncertain)
        if (cascade_source in ("fallback_rule", "fallback_coldstart") and
                confidence < self.trigger_cfg.low_confidence_trigger):
            return "low_confidence"

        # RMS threshold trigger (ISO 10816 Zone D)
        if overall_rms > self.trigger_cfg.rms_threshold:
            return "rms_high"

        return None
