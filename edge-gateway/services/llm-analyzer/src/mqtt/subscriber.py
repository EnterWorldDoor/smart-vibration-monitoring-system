"""MQTT subscriber with trigger detection and deduplication.

Deduplication logic:
  - Key: (site_id, device_id, severity)
  - Window: 5 minutes (configurable)
  - Escalation: severity change (WARNING→CRITICAL or CRITICAL→WARNING)
    bypasses dedup and generates a new report immediately.
"""

import json
import time
import structlog
from dataclasses import dataclass, field
from typing import Callable, Optional

import paho.mqtt.client as mqtt

from src.config import MQTTConfig, DedupConfig

logger = structlog.get_logger(__name__)

# Severity priority for escalation comparison
_SEVERITY_ORDER = {"NORMAL": 0, "WARNING": 1, "CRITICAL": 2}


@dataclass
class TriggerEvent:
    topic: str = ""
    site_id: str = ""
    device_id: str = ""
    severity: str = "WARNING"
    trigger_reason: str = ""
    payload: dict = field(default_factory=dict)
    timestamp: float = 0.0


@dataclass
class _DedupEntry:
    severity: str
    last_triggered: float  # monotonic seconds


class MQTTSubscriber:
    """Subscribes to inference-engine reports, applies dedup, fires callbacks."""

    def __init__(self, mqtt_cfg: MQTTConfig, dedup_cfg: DedupConfig):
        self.mqtt_cfg = mqtt_cfg
        self.dedup_cfg = dedup_cfg
        self.client = mqtt.Client(client_id=mqtt_cfg.client_id, protocol=mqtt.MQTTv311)
        self._trigger_callback: Optional[Callable[[TriggerEvent], None]] = None
        self._connected = False

        # In-memory dedup state: {(site_id, device_id, severity): _DedupEntry}
        self._dedup: dict[tuple, _DedupEntry] = {}
        self._total_dedup_hits = 0
        self._total_triggers = 0

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)

    @property
    def total_triggers(self) -> int:
        return self._total_triggers

    @property
    def total_dedup_hits(self) -> int:
        return self._total_dedup_hits

    def set_trigger_callback(self, cb: Callable[[TriggerEvent], None]):
        self._trigger_callback = cb

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
        else:
            logger.error("mqtt connect failed", rc=rc)

    def _on_disconnect(self, client, userdata, rc):
        self._connected = False
        if rc != 0:
            logger.warning("mqtt disconnected unexpectedly", rc=rc)

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload)
        except json.JSONDecodeError:
            return

        topic_parts = msg.topic.split("/")
        if len(topic_parts) < 5:
            return

        site_id = topic_parts[1]
        # topic_parts[2] is "inference" (fixed)
        device_id = topic_parts[3]

        severity = payload.get("severity", "").upper()
        if severity not in ("WARNING", "CRITICAL"):
            return

        trigger_reason = payload.get("trigger_reason", "unknown")

        if not self._check_dedup(site_id, device_id, severity):
            self._total_dedup_hits += 1
            return

        self._total_triggers += 1
        event = TriggerEvent(
            topic=msg.topic,
            site_id=site_id,
            device_id=device_id,
            severity=severity,
            trigger_reason=trigger_reason,
            payload=payload,
            timestamp=time.monotonic(),
        )

        if self._trigger_callback:
            self._trigger_callback(event)

    def _check_dedup(self, site_id: str, device_id: str, severity: str) -> bool:
        """Returns True if this trigger should be processed (not a duplicate)."""
        self._cleanup_expired()
        now = time.monotonic()
        key = (site_id, device_id)
        existing = self._dedup.get(key)

        if existing is None:
            self._dedup[key] = _DedupEntry(severity=severity, last_triggered=now)
            return True

        # Same severity within window → suppress
        if severity == existing.severity:
            if now - existing.last_triggered < self.dedup_cfg.window_seconds:
                logger.debug("dedup suppressed",
                             device_id=device_id, severity=severity)
                return False
            # Window expired → allow, update entry
            existing.severity = severity
            existing.last_triggered = now
            return True

        # Different severity → escalation/de-escalation, always penetrate
        if self.dedup_cfg.escalation_penetrate:
            logger.info("severity change penetrates dedup",
                        device_id=device_id,
                        old=existing.severity, new=severity)
            existing.severity = severity
            existing.last_triggered = now
            return True

        # Escalation disabled: treat as normal dedup check
        if now - existing.last_triggered < self.dedup_cfg.window_seconds:
            return False
        existing.severity = severity
        existing.last_triggered = now
        return True

    def _cleanup_expired(self):
        """Remove dedup entries past their window. Called on each check."""
        now = time.monotonic()
        expired = [
            key for key, entry in self._dedup.items()
            if now - entry.last_triggered >= self.dedup_cfg.window_seconds * 2
        ]
        for key in expired:
            del self._dedup[key]
