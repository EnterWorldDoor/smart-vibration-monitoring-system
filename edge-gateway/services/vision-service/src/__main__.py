"""
vision-service — USB camera periodic capture with MQTT event triggering.

Architecture:
  Synchronous main loop (while+sleep) with queue.Queue bridging MQTT events.
  cv2.VideoCapture NOT thread-safe -> all camera ops happen in main thread.
"""

import argparse
import queue
import signal
import sys
import time
from datetime import datetime, timezone

import structlog

from src.config import Config, DeviceConfig
from src.camera.capture import CameraCapture
from src.storage.file_store import (
    build_file_path, ensure_dir, write_jpeg,
    rotate_expired_files,
)
from src.db.client import VisonDBClient
from src.mqtt.publisher import MQTTPublisher
from src.mqtt.subscriber import MQTTSubscriber, TriggerEvent
from src.health import HealthReporter

logger = structlog.get_logger("vision-service")

QUEUE_MAXSIZE = 32
ROTATION_INTERVAL_S = 3600
HEALTH_INTERVAL_S = 30


class VisionService:
    def __init__(self, config_path: str):
        self.cfg = Config.from_yaml(config_path)
        self._running = False
        self._event_queue: queue.Queue[tuple[TriggerEvent, DeviceConfig]] = \
            queue.Queue(maxsize=QUEUE_MAXSIZE)

        self._cameras: dict[int, CameraCapture] = {}
        self._db = VisonDBClient(self.cfg.timescaledb)
        self._mqtt_pub = MQTTPublisher(self.cfg.mqtt)
        self._mqtt_sub = MQTTSubscriber(self.cfg.mqtt, self.cfg.devices)
        self._health = HealthReporter(self.cfg.mqtt, self.cfg.primary_site_id())

    def start(self):
        logger.info("vision-service starting", devices=len(self.cfg.devices))

        for dev in self.cfg.devices:
            cam = CameraCapture(dev.camera_index, self.cfg.capture)
            if cam.open():
                self._cameras[dev.camera_index] = cam
            else:
                logger.warning("camera not available -- will retry each cycle",
                               camera_index=dev.camera_index, label=dev.label)

        self._db.connect()
        self._mqtt_pub.connect()
        self._mqtt_sub.set_trigger_callback(self._on_trigger)
        self._mqtt_sub.connect()
        self._health.connect()

        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                signal.signal(sig, self._shutdown_handler)
            except Exception:
                pass

        self._running = True
        last_baseline = time.time()
        last_rotation = time.time()
        last_health = time.time()

        logger.info("vision-service running",
                    baseline_interval_s=self.cfg.capture.baseline_interval_s,
                    devices=[d.device_id for d in self.cfg.devices])

        while self._running:
            try:
                self._drain_events()

                now = time.time()
                if now - last_baseline >= self.cfg.capture.baseline_interval_s:
                    self._capture_all_baselines()
                    last_baseline = now

                if now - last_rotation >= ROTATION_INTERVAL_S:
                    self._rotate_all()
                    last_rotation = now

                if now - last_health >= HEALTH_INTERVAL_S:
                    self._health.report()
                    last_health = now

            except Exception:
                logger.exception("main loop iteration error")
                self._health.errors += 1

            time.sleep(1)

        self._shutdown()

    def _shutdown_handler(self, signum, frame):
        logger.info("signal received", signal=signum)
        self._running = False

    def _shutdown(self):
        logger.info("shutting down")
        for cam in self._cameras.values():
            cam.release()
        self._mqtt_sub.disconnect()
        self._mqtt_pub.disconnect()
        self._health.disconnect()
        self._db.close()
        logger.info("shutdown complete")

    def _on_trigger(self, event: TriggerEvent, dev: DeviceConfig):
        """Called from paho thread. Push to queue for main-thread processing."""
        try:
            self._event_queue.put_nowait((event, dev))
        except queue.Full:
            logger.debug("event queue full, dropping trigger",
                         device_id=event.device_id)

    def _drain_events(self):
        while not self._event_queue.empty():
            try:
                event, dev = self._event_queue.get_nowait()
                self._capture_event(event, dev)
            except queue.Empty:
                break
            except Exception:
                logger.exception("event capture failed")
                self._health.errors += 1

    def _capture_all_baselines(self):
        timestamp = datetime.now(timezone.utc)
        resolution = self.cfg.capture.baseline_width_height()
        for dev in self.cfg.devices:
            try:
                cam = self._get_or_open_camera(dev)
                if cam is None:
                    continue

                frame = cam.read_frame(resolution)
                if frame is None:
                    logger.warning("baseline frame read failed", device=dev.device_id)
                    continue

                jpeg_bytes = cam.encode_jpeg(frame, self.cfg.capture.baseline_quality)
                file_path = build_file_path(
                    self.cfg.capture.storage_path,
                    dev.site_id, dev.device_id,
                    capture_type="baseline",
                    timestamp=timestamp,
                )
                ensure_dir(file_path)
                write_jpeg(file_path, jpeg_bytes)
                file_size = len(jpeg_bytes)

                self._db.insert_capture(
                    timestamp=timestamp, site_id=dev.site_id,
                    device_id=dev.device_id, capture_type="baseline",
                    trigger_src="timer",
                    resolution=self.cfg.capture.baseline_resolution,
                    file_path=file_path, file_size_bytes=file_size,
                )

                self._mqtt_pub.publish_capture(
                    site_id=dev.site_id, device_id=dev.device_id,
                    capture_type="baseline", file_path=file_path,
                    resolution=self.cfg.capture.baseline_resolution,
                    file_size_bytes=file_size, timestamp_iso=timestamp.isoformat(),
                )

                self._health.total_baselines += 1
                logger.debug("baseline captured", device=dev.device_id, path=file_path)

            except Exception:
                logger.exception("baseline capture error", device=dev.device_id)
                self._health.errors += 1

    def _capture_event(self, event: TriggerEvent, dev: DeviceConfig):
        timestamp = datetime.now(timezone.utc)
        resolution = self.cfg.capture.event_width_height()
        try:
            cam = self._get_or_open_camera(dev)
            if cam is None:
                return

            frame = cam.read_frame(resolution)
            if frame is None:
                logger.warning("event frame read failed", device=dev.device_id,
                               severity=event.severity)
                return

            jpeg_bytes = cam.encode_jpeg(frame, self.cfg.capture.event_quality)
            file_path = build_file_path(
                self.cfg.capture.storage_path,
                dev.site_id, dev.device_id,
                capture_type="event",
                timestamp=timestamp,
                severity=event.severity,
            )
            ensure_dir(file_path)
            write_jpeg(file_path, jpeg_bytes)
            file_size = len(jpeg_bytes)

            self._db.insert_capture(
                timestamp=timestamp, site_id=dev.site_id,
                device_id=dev.device_id, capture_type="event",
                trigger_src="mqtt_inference",
                resolution=self.cfg.capture.event_resolution,
                file_path=file_path, file_size_bytes=file_size,
            )

            self._mqtt_pub.publish_capture(
                site_id=dev.site_id, device_id=dev.device_id,
                capture_type="event", file_path=file_path,
                resolution=self.cfg.capture.event_resolution,
                file_size_bytes=file_size, timestamp_iso=timestamp.isoformat(),
            )

            self._health.total_events += 1
            logger.info("event captured", device=dev.device_id,
                        severity=event.severity, path=file_path)

        except Exception:
            logger.exception("event capture error", device=dev.device_id,
                             severity=event.severity)
            self._health.errors += 1

    def _get_or_open_camera(self, dev: DeviceConfig):
        cam = self._cameras.get(dev.camera_index)
        if cam is not None and cam.is_open():
            return cam
        new_cam = CameraCapture(dev.camera_index, self.cfg.capture)
        if new_cam.open():
            self._cameras[dev.camera_index] = new_cam
            return new_cam
        logger.warning("camera unavailable -- skipping capture",
                       camera_index=dev.camera_index, device=dev.device_id)
        return None

    def _rotate_all(self):
        base_path = self.cfg.capture.storage_path
        try:
            rotate_expired_files(base_path, self.cfg.capture.baseline_retention_days)
            rotate_expired_files(base_path, self.cfg.capture.event_retention_days)
        except Exception:
            logger.exception("rotation error")
            self._health.errors += 1


def main():
    parser = argparse.ArgumentParser(description="EdgeVib Vision Service")
    parser.add_argument("--config", default="config.yaml",
                        help="Path to config YAML")
    args = parser.parse_args()

    structlog.configure(
        processors=[
            structlog.processors.TimeStamper(fmt="iso"),
            structlog.dev.ConsoleRenderer(),
        ],
        context_class=dict,
        logger_factory=structlog.PrintLoggerFactory(),
        cache_logger_on_first_use=True,
    )

    service = VisionService(args.config)
    try:
        service.start()
    except KeyboardInterrupt:
        service._shutdown()


if __name__ == "__main__":
    main()
