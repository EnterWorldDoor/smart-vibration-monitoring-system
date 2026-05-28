"""audio-monitor — industrial acoustic anomaly detection service.

Architecture:
  Portaudio C-thread callback (every ~128 ms): FFT → features → queue.put
  Python main thread: queue.get → baseline → anomaly → DB batch → MQTT

Pattern mirrored from vision-service src/__main__.py (sync while+sleep loop).
"""

import argparse
import os
import signal
import sys
import time
from datetime import datetime, timezone
from queue import Queue, Empty
from typing import Optional

import structlog

try:
    import numpy as np
except ImportError:
    pass  # handled at open() time

from src.config import Config
from src.audio.capture import AudioCapture, AudioFrame
from src.audio.processor import AudioProcessor
from src.anomaly.detector import ThresholdDetector, AnomalyResult
from src.db.client import AudioDBClient
from src.health import HealthReporter
from src.mqtt.publisher import MQTTPublisher
from src.mqtt.subscriber import MQTTSubscriber
from src.storage.file_store import (
    PreTriggerBuffer, build_wav_path, build_baseline_path,
    ensure_dir, write_wav, rotate_expired_anomalies, rotate_baselines,
)

logger = structlog.get_logger("audio-monitor")

# ---------------------------------------------------------------------------
# Main service class
# ---------------------------------------------------------------------------

class AudioMonitorService:
    def __init__(self, config_path: str):
        self.cfg = Config.from_yaml(config_path)
        self._running = False
        self._start_time: Optional[float] = None

        site_id = self.cfg.primary_site_id()
        device_id = self.cfg.primary_device_id()

        # ── Core processing pipeline ──
        self._processor = AudioProcessor(
            sample_rate=self.cfg.audio.sample_rate,
            block_size=self.cfg.audio.block_size,
            baseline_window_s=self.cfg.alarm.baseline_window_s,
            learning_period_s=self.cfg.alarm.learning_period_s,
        )
        self._detector = ThresholdDetector(
            sigma_warning=self.cfg.alarm.sigma_warning,
            sigma_critical=self.cfg.alarm.sigma_critical,
            anomaly_sustain_s=self.cfg.alarm.anomaly_sustain_s,
            learning_period_s=self.cfg.alarm.learning_period_s,
        )
        self._pre_trigger = PreTriggerBuffer(
            duration_s=self.cfg.alarm.pre_trigger_s,
            sample_rate=self.cfg.audio.sample_rate,
            block_size=self.cfg.audio.block_size,
        )

        # ── Audio capture ──
        self._capture = AudioCapture(
            processor=self._processor,
            sample_rate=self.cfg.audio.sample_rate,
            block_size=self.cfg.audio.block_size,
            channels=self.cfg.audio.channels,
            dtype=self.cfg.audio.dtype,
            device_index=self.cfg.audio.device_index,
        )

        # ── I/O modules ──
        self._db = AudioDBClient(self.cfg.timescaledb)
        self._mqtt_pub = MQTTPublisher(self.cfg.mqtt)
        self._mqtt_sub = MQTTSubscriber(self.cfg.mqtt, self.cfg.devices)
        self._health = HealthReporter(self.cfg.mqtt, site_id)

        # Post-trigger state machine
        self._post_trigger_blocks_left: int = 0
        self._post_trigger_buffer: list[np.ndarray] = []
        self._pending_anomaly: Optional[AnomalyResult] = None
        self._pending_baseline_rms: float = 0.0
        self._pending_anomaly_start_time: float = 0.0

        # Periodic timer bookkeeping
        self._last_rotation: float = 0.0
        self._last_health: float = 0.0
        self._last_baseline_wav: float = 0.0

        logger.info("audio-monitor initialised",
                    site_id=site_id, device_id=device_id,
                    sample_rate=self.cfg.audio.sample_rate,
                    block_size=self.cfg.audio.block_size)

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self) -> int:
        # 1. Enumerate devices
        devices = AudioCapture.enumerate_devices()
        logger.info("audio input devices", count=len(devices))
        for d in devices:
            logger.info("  device", index=d["index"], name=d["name"])

        # 2. Validate storage path
        ensure_dir(os.path.join(self.cfg.audio.storage_path, "anomalies"))
        ensure_dir(os.path.join(self.cfg.audio.storage_path, "baselines"))

        # 3. Connect I/O
        self._db.connect()
        self._mqtt_pub.connect()
        self._mqtt_sub.connect()
        self._health.connect()

        # 4. Open audio stream
        if not self._capture.open():
            logger.warning("audio capture not available at startup; "
                           "will retry in main loop")

        # 5. Signal handlers
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                signal.signal(sig, self._signal_handler)
            except ValueError:
                pass

        # 6. Main loop
        self._running = True
        self._start_time = time.time()
        self._last_rotation = time.time()
        self._last_health = time.time()
        self._last_baseline_wav = time.time()

        try:
            self._main_loop()
        except KeyboardInterrupt:
            pass
        finally:
            self._shutdown()

        return 0

    def _signal_handler(self, signum, frame):
        logger.info("signal received, shutting down", signal=signum)
        self._running = False

    def _shutdown(self):
        logger.info("shutting down")
        self._running = False
        self._capture.close()
        self._db.flush_features()
        self._mqtt_pub.disconnect()
        self._mqtt_sub.disconnect()
        self._health.disconnect()
        self._db.close()
        logger.info("stopped")

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    def _main_loop(self):
        while self._running:
            try:
                self._tick()
            except Exception:
                logger.exception("main loop error")
                self._health.errors += 1
                time.sleep(0.5)

    def _tick(self):
        """One iteration of the main loop.  Non-blocking."""
        # ── Try reopening capture if needed (every 5 s retry) ──
        if not self._capture.is_active():
            now = time.time()
            if self._start_time and (now - self._start_time) % 5.0 < 0.15:
                self._capture.open()

        # ── 1. Process next audio frame ──
        frame = self._next_frame()
        if frame is not None:
            self._process_frame(frame)

        # ── 2. Flush DB batch ──
        if self._db.should_flush():
            written = self._db.flush_features()
            if written == 0 and self._db.is_connected():
                self._health.db_write_failures += 1

        # ── 3. Drain MQTT trigger events ──
        self._drain_mqtt_events()

        # ── 4. Periodic tasks ──
        now = time.time()
        if now - self._last_health >= 30:
            self._health.report()
            self._last_health = now

        if now - self._last_baseline_wav >= 600:
            self._save_baseline_wav()
            self._last_baseline_wav = now

        if now - self._last_rotation >= 3600:
            rotate_expired_anomalies(self.cfg.audio.storage_path)
            rotate_baselines(self.cfg.audio.storage_path)
            self._last_rotation = now

    # ------------------------------------------------------------------
    # Frame processing
    # ------------------------------------------------------------------

    def _next_frame(self) -> Optional[AudioFrame]:
        try:
            return self._capture.get_queue().get(timeout=0.1)
        except Empty:
            return None

    def _process_frame(self, frame: AudioFrame) -> None:
        # ── Ring buffer: always push ──
        self._pre_trigger.push(frame.raw_audio)

        # ── Post-trigger collection mode ──
        if self._post_trigger_blocks_left > 0:
            self._post_trigger_buffer.append(frame.raw_audio.copy())
            self._post_trigger_blocks_left -= 1
            if self._post_trigger_blocks_left == 0:
                self._finalize_anomaly_save()
            # Don't update baseline or check for new anomalies during
            # post-trigger collection
            return

        # ── Baseline update (only when not in anomaly) ──
        if self._pending_anomaly is None and not self._in_learning():
            self._processor.update_baseline(frame)

        # ── Anomaly check ──
        if self._pending_anomaly is None and not self._in_learning():
            result = self._detector.evaluate(
                frame, self._processor.baseline
            )
            if result is not None:
                self._start_anomaly_capture(result, frame)

        # ── Buffer for DB ──
        if self._db.is_connected():
            self._db.buffer_feature(
                datetime.now(timezone.utc),
                self.cfg.primary_site_id(),
                self.cfg.primary_device_id(),
                frame,
            )
        else:
            self._health.db_write_failures += 1

        self._health.frames_processed += 1

    def _in_learning(self) -> bool:
        """True during the initial learning period."""
        if self._start_time is None:
            return True
        return (time.time() - self._start_time) < self.cfg.alarm.learning_period_s

    # ------------------------------------------------------------------
    # Anomaly state machine
    # ------------------------------------------------------------------

    def _start_anomaly_capture(self, result: AnomalyResult,
                                frame: AudioFrame) -> None:
        self._pending_anomaly = result
        self._pending_baseline_rms = self._processor.baseline.get(
            "rms_mean", 0.0
        )
        self._pending_anomaly_start_time = time.time()

        blocks_needed = int(
            self.cfg.alarm.post_trigger_s
            * self.cfg.audio.sample_rate
            / self.cfg.audio.block_size
        )
        self._post_trigger_blocks_left = max(blocks_needed, 1)
        self._post_trigger_buffer.clear()

        logger.info("anomaly started",
                    severity=result.severity,
                    reason=result.trigger_reason,
                    sigma=round(result.sigma_level, 2),
                    post_trigger_blocks=blocks_needed)

    def _finalize_anomaly_save(self) -> None:
        if self._pending_anomaly is None:
            return

        result = self._pending_anomaly
        now = datetime.now(timezone.utc)
        device_id = self.cfg.primary_device_id()
        base = self.cfg.audio.storage_path

        # 1. Collect pre + post trigger audio
        pre_audio = self._pre_trigger.drain()
        post_audio = np.concatenate(
            self._post_trigger_buffer, dtype=np.int16
        ) if self._post_trigger_buffer else np.array([], dtype=np.int16)
        combined = np.concatenate([pre_audio, post_audio])
        duration_ms = int(len(combined) / self.cfg.audio.sample_rate * 1000)

        # 2. Write WAV
        wav_path = build_wav_path(base, device_id, now)
        ok = write_wav(wav_path, combined, self.cfg.audio.sample_rate)

        # 3. DB insert anomaly record
        self._db.insert_anomaly(
            ts=now, site_id=self.cfg.primary_site_id(),
            device_id=device_id,
            severity=result.severity,
            trigger_reason=result.trigger_reason,
            rms_energy=result.features.get("rms_energy", 0.0),
            baseline_rms=self._pending_baseline_rms,
            sigma_level=result.sigma_level,
            wav_path=wav_path if ok else "",
            duration_ms=duration_ms,
            metadata={
                "spectral_centroid_hz": result.features.get(
                    "spectral_centroid_hz", 0.0
                ),
                "spectral_kurtosis": result.features.get(
                    "spectral_kurtosis", 0.0
                ),
                "hf_lf_ratio": result.features.get("hf_lf_ratio", 0.0),
                "dominant_freq_hz": result.features.get(
                    "dominant_freq_hz", 0.0
                ),
                "dominant_amp_db": result.features.get(
                    "dominant_amp_db", 0.0
                ),
            },
        )

        # 4. MQTT publish alert
        self._mqtt_pub.publish_alert(
            site_id=self.cfg.primary_site_id(),
            device_id=device_id,
            severity=result.severity,
            trigger_reason=result.trigger_reason,
            rms_energy=result.features.get("rms_energy", 0.0),
            baseline_rms=self._pending_baseline_rms,
            sigma_level=result.sigma_level,
            spectral_centroid_hz=result.features.get(
                "spectral_centroid_hz", 0.0
            ),
            spectral_kurtosis=result.features.get("spectral_kurtosis", 0.0),
            wav_path=wav_path if ok else "(write failed)",
            timestamp_iso=now.isoformat(),
        )

        self._health.anomalies_detected += 1
        if ok:
            self._health.wav_files_written += 1

        logger.info("anomaly saved",
                    severity=result.severity,
                    wav_path=wav_path if ok else "FAILED",
                    duration_ms=duration_ms)

        # 5. Reset state
        self._pending_anomaly = None
        self._post_trigger_buffer.clear()
        self._pre_trigger.clear()

    # ------------------------------------------------------------------
    # Background tasks
    # ------------------------------------------------------------------

    def _drain_mqtt_events(self) -> None:
        """Non-blocking drain of MQTT trigger events from paho thread."""
        q = self._mqtt_sub.get_queue()
        while True:
            try:
                event, dev = q.get_nowait()
            except Empty:
                break
            logger.debug("mqtt trigger received",
                         site_id=event.site_id,
                         device_id=event.device_id,
                         severity=event.severity)

    def _save_baseline_wav(self) -> None:
        """Write current pre-trigger buffer as a baseline WAV for reference."""
        base = self.cfg.audio.storage_path
        device_id = self.cfg.primary_device_id()
        audio = self._pre_trigger.drain()
        if len(audio) == 0:
            return
        # Take 2 s worth
        n_samples = self.cfg.audio.sample_rate * 2
        snippet = audio[:n_samples] if len(audio) > n_samples else audio
        path = build_baseline_path(
            base, device_id, datetime.now(timezone.utc)
        )
        write_wav(path, snippet, self.cfg.audio.sample_rate)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="EdgeVib Audio Monitor Service"
    )
    parser.add_argument(
        "--config", "-c", default="config/audio-monitor.yaml",
        help="Path to YAML config file",
    )
    args = parser.parse_args()

    # Structured logging
    try:
        structlog.configure(
            processors=[
                structlog.processors.TimeStamper(fmt="iso"),
                structlog.dev.ConsoleRenderer(),
            ],
            context_class=dict,
            logger_factory=structlog.PrintLoggerFactory(),
            cache_logger_on_first_use=True,
        )
    except Exception:
        pass

    svc = AudioMonitorService(args.config)
    return svc.start()


if __name__ == "__main__":
    sys.exit(main())
