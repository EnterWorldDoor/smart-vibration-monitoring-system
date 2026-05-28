"""MQTT alert publisher.  Pattern mirrored from vision-service mqtt/publisher.py."""

import json

import paho.mqtt.client as mqtt
import structlog

from src.config import MQTTConfig

logger = structlog.get_logger(__name__)


class MQTTPublisher:
    """Thin wrapper around paho MQTT for publishing audio alerts."""

    def __init__(self, cfg: MQTTConfig):
        self.cfg = cfg
        self.client = mqtt.Client(
            client_id=f"{cfg.client_id}-pub", protocol=mqtt.MQTTv311,
        )

    def connect(self) -> None:
        try:
            self.client.connect(self.cfg.broker, self.cfg.port, 60)
            self.client.loop_start()
            logger.info("mqtt publisher connected", broker=self.cfg.broker)
        except Exception:
            logger.warning("mqtt publisher connect failed",
                           broker=self.cfg.broker)

    def disconnect(self) -> None:
        self.client.loop_stop()
        try:
            self.client.disconnect()
        except Exception:
            pass

    def publish_alert(
        self, site_id: str, device_id: str, severity: str,
        trigger_reason: str, rms_energy: float, baseline_rms: float,
        sigma_level: float, spectral_centroid_hz: float,
        spectral_kurtosis: float, wav_path: str, timestamp_iso: str,
    ) -> None:
        topic = self.cfg.publish_topic.format(
            site_id=site_id, device_id=device_id
        )
        payload = json.dumps({
            "device_id": device_id,
            "timestamp_utc": timestamp_iso,
            "severity": severity,
            "trigger_reason": trigger_reason,
            "rms_energy": round(rms_energy, 6),
            "baseline_rms": round(baseline_rms, 6),
            "sigma_level": round(sigma_level, 2),
            "spectral_centroid_hz": round(spectral_centroid_hz, 1),
            "spectral_kurtosis": round(spectral_kurtosis, 2),
            "wav_path": wav_path,
        })
        try:
            info = self.client.publish(
                topic, payload, qos=self.cfg.qos, retain=False,
            )
            if info.rc == mqtt.MQTT_ERR_SUCCESS:
                logger.debug("alert published", topic=topic,
                             severity=severity)
            else:
                logger.warning("publish failed", topic=topic, rc=info.rc)
        except Exception:
            logger.warning("publish exception", topic=topic)
