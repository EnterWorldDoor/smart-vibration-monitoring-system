"""
inference-engine — Edge-AI inference service for vibration monitoring.

Architecture:
  Main loop (asyncio with run_in_executor for sync DB calls):
    1. Every 10s: Query TimescaleDB for recent features → trend analysis + AE detection
    2. MQTT trigger: Instant inference on bearing_fault / low_confidence / high_RMS
    3. Results: Write to ai_reports table + MQTT publish key findings
"""

import asyncio
import concurrent.futures
import signal
import time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import structlog

from src.config import Config
from src.db.client import DBClient
from src.health import HealthReporter
from src.inference.aggregation import compute_motor_health
from src.inference.autoencoder import Autoencoder
from src.inference.feature_extractor import FeatureExtractor
from src.inference.trend_analysis import TrendAnalyzer, TrendResult
from src.mqtt.publisher import MQTTPublisher
from src.mqtt.subscriber import MQTTSubscriber, TriggerEvent

logger = structlog.get_logger("inference-engine")


class InferenceEngine:
    def __init__(self, config_path: str = "config.yaml"):
        self.cfg = Config.from_yaml(config_path)
        model_dir = Path(__file__).resolve().parent

        self.autoencoder = Autoencoder(
            str(model_dir / self.cfg.model.path),
            str(model_dir / self.cfg.model.metadata_path),
        )
        self.feature_extractor = FeatureExtractor.from_metadata_file(
            str(model_dir / self.cfg.model.metadata_path)
        )
        self.trend_analyzer = TrendAnalyzer(self.cfg.trend)

        self.db = DBClient(self.cfg.db)
        self.mqtt_sub = MQTTSubscriber(self.cfg.mqtt, self.cfg.trigger)
        self.mqtt_pub = MQTTPublisher(self.cfg.mqtt)
        self.health = HealthReporter(self.cfg.mqtt, self.cfg.site_id)

        self._running = False
        self._trigger_queue: asyncio.Queue[TriggerEvent] = asyncio.Queue(maxsize=64)
        self._executor = concurrent.futures.ThreadPoolExecutor(max_workers=2)

    async def start(self):
        logger.info("inference-engine starting", site_id=self.cfg.site_id)
        self.autoencoder.load()
        self.db.connect()
        self.mqtt_sub.set_trigger_callback(self._on_trigger)
        self.mqtt_sub.connect()
        self.mqtt_pub.connect()
        self.health.connect()

        self._running = True
        tasks = [
            asyncio.create_task(self._main_loop()),
            asyncio.create_task(self._trigger_consumer()),
            asyncio.create_task(self._health_loop()),
        ]
        logger.info("inference-engine running",
                    interval_s=self.cfg.schedule.inference_interval_s)

        loop = asyncio.get_event_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, self._shutdown)
            except NotImplementedError:
                pass  # Windows doesn't support add_signal_handler

        try:
            await asyncio.gather(*tasks)
        except asyncio.CancelledError:
            pass

    def _shutdown(self):
        logger.info("shutting down")
        self._running = False
        self.mqtt_sub.disconnect()
        self.mqtt_pub.disconnect()
        self.health.disconnect()
        self.db.close()
        self._executor.shutdown(wait=False)

    def _on_trigger(self, event: TriggerEvent):
        try:
            self._trigger_queue.put_nowait(event)
        except asyncio.QueueFull:
            logger.debug("trigger queue full", reason=event.trigger_reason)

    async def _main_loop(self):
        interval = self.cfg.schedule.inference_interval_s
        while self._running:
            try:
                await self._run_inference_cycle()
            except Exception:
                logger.exception("inference cycle error")
                self.health.errors += 1
            await asyncio.sleep(interval)

    async def _trigger_consumer(self):
        while self._running:
            try:
                event = await asyncio.wait_for(
                    self._trigger_queue.get(), timeout=1.0)
                await self._handle_trigger(event)
            except asyncio.TimeoutError:
                continue
            except Exception:
                logger.exception("trigger handler error")
                self.health.errors += 1

    async def _health_loop(self):
        interval = self.cfg.schedule.health_report_interval_s
        while self._running:
            await asyncio.sleep(interval)
            try:
                self.health.report()
            except Exception:
                logger.exception("health report error")

    async def _run_inference_cycle(self):
        """Main timed inference: query DB → analyze → write results."""
        t0 = time.perf_counter()
        loop = asyncio.get_event_loop()

        devices = await loop.run_in_executor(
            self._executor,
            self.db.query_distinct_devices,
            self.cfg.site_id,
        )

        for dev in (devices or []):
            device_type = dev["device_type"]
            device_id = dev["device_id"]

            # Query all data in parallel via executor
            feature_rows, vibration_rows, dual_rows = await asyncio.gather(
                loop.run_in_executor(self._executor,
                    self.db.query_recent_features,
                    self.cfg.site_id, device_id, self.cfg.trend.window_size),
                loop.run_in_executor(self._executor,
                    self.db.query_recent_vibration,
                    self.cfg.site_id, device_id, self.cfg.trend.window_size),
                loop.run_in_executor(self._executor,
                    self.db.query_dual_channel,
                    self.cfg.site_id, device_id, self.cfg.trend.window_size),
            )

            if not feature_rows and not vibration_rows:
                continue

            # Autoencoder inference (CPU — runs in executor to avoid blocking event loop)
            anomaly_score = None
            if feature_rows:
                features = self.feature_extractor.extract_from_view_rows(feature_rows)
                X = self.feature_extractor.normalize_list(features)
                scores = await loop.run_in_executor(
                    self._executor, self.autoencoder.anomaly_scores, X)
                anomaly_score = float(np.mean(scores[-5:])) if len(scores) >= 5 else float(np.mean(scores))
                self.health.total_inferences += 1
                # Sanity check: if anomaly_score > 100, model calibration is likely wrong
                # (training data distribution doesn't match ESP32 features).
                # Fall back to trend-only health scoring.
                if anomaly_score > 100.0:
                    logger.warning("autoencoder uncalibrated — using trends only",
                                   anomaly_score=anomaly_score)
                    anomaly_score = None
                elif np.any(scores[-3:] > self.autoencoder.get_threshold()):
                    self.health.total_anomalies += 1

            # Trend analysis (pure numpy/scipy — fast enough to run inline)
            trend = self.trend_analyzer.analyze(vibration_rows, dual_rows, feature_rows)

            # Motor health aggregation
            motor_id = f"{device_type}-{device_id}"
            motor_health = compute_motor_health(
                motor_id, trend, None, anomaly_score, None)

            # Write to DB
            elapsed = (time.perf_counter() - t0) * 1000
            self.health.last_inference_time_ms = elapsed
            severity = motor_health.severity

            await loop.run_in_executor(self._executor, self.db.insert_report,
                datetime.now(timezone.utc),
                self.cfg.site_id, "motor_health", device_id, severity,
                {
                    "health_score": motor_health.health_score,
                    "anomaly_detected": motor_health.anomaly_detected,
                    "anomaly_score": anomaly_score,
                    "rms_slope": trend.rms_slope,
                    "freq_drift_std": trend.freq_drift_std,
                    "de_nde_ratio": trend.de_nde_ratio,
                    "warnings": trend.warnings,
                    "overall_status": trend.overall_status,
                },
                self.autoencoder.metadata["model_name"],
                self.autoencoder.metadata["version"],
                anomaly_score, motor_health.health_score, elapsed,
                motor_health.details,
            )
            self.health.total_reports += 1

            # Publish key findings to MQTT if not normal
            if severity in ("WARNING", "CRITICAL"):
                await loop.run_in_executor(self._executor,
                    self.mqtt_pub.publish_report,
                    self.cfg.site_id, device_id, {
                        "anomaly_detected": motor_health.anomaly_detected,
                        "health_score": motor_health.health_score,
                        "severity": severity,
                        "anomaly_score": anomaly_score,
                        "summary": motor_health.summary,
                        "warnings": trend.warnings,
                        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                    })

    async def _handle_trigger(self, event: TriggerEvent):
        """Handle MQTT trigger: instant inference with retrospective context."""
        logger.info("trigger received", device=event.device_id,
                    reason=event.trigger_reason)
        loop = asyncio.get_event_loop()

        feature_rows = await loop.run_in_executor(self._executor,
            self.db.query_recent_features,
            event.site_id, event.device_id, self.cfg.trend.window_size)

        if not feature_rows:
            logger.debug("no features for trigger", device=event.device_id)
            return

        features = self.feature_extractor.extract_from_view_rows(feature_rows)
        X = self.feature_extractor.normalize_list(features)
        scores = await loop.run_in_executor(
            self._executor, self.autoencoder.anomaly_scores, X)
        anomaly_score = float(np.mean(scores[-5:])) if len(scores) >= 5 else float(np.mean(scores))
        self.health.total_inferences += 1

        is_anomaly = bool(np.any(scores[-3:] > self.autoencoder.get_threshold()))
        severity = "CRITICAL" if is_anomaly else "WARNING"
        if is_anomaly:
            self.health.total_anomalies += 1

        await loop.run_in_executor(self._executor, self.db.insert_report,
            datetime.now(timezone.utc),
            event.site_id, "anomaly_detection", event.device_id, severity,
            {
                "trigger_reason": event.trigger_reason,
                "anomaly_score": anomaly_score,
                "anomaly_detected": is_anomaly,
                "samples_analyzed": len(feature_rows),
            },
            self.autoencoder.metadata["model_name"],
            self.autoencoder.metadata["version"],
            anomaly_score, None, 0.0, None,
        )
        self.health.total_reports += 1

        await loop.run_in_executor(self._executor,
            self.mqtt_pub.publish_report,
            event.site_id, event.device_id, {
                "trigger_reason": event.trigger_reason,
                "anomaly_detected": is_anomaly,
                "anomaly_score": anomaly_score,
                "severity": severity,
                "summary": f"Trigger: {event.trigger_reason}, "
                          f"anomaly={'yes' if is_anomaly else 'no'}",
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            })


def main():
    import argparse
    parser = argparse.ArgumentParser(description="EdgeVib Inference Engine")
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

    engine = InferenceEngine(args.config)
    try:
        asyncio.run(engine.start())
    except KeyboardInterrupt:
        engine._shutdown()


if __name__ == "__main__":
    main()
