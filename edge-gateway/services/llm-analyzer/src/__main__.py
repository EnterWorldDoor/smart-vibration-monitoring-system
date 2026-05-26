"""
llm-analyzer — Local LLM fault report generation for EdgeVib.

Architecture:
  Trigger path: MQTT EdgeVib/+/inference/+/ai/report (WARNING/CRITICAL)
    → dedup check → DB context query → prompt build → llama.cpp → DB + MQTT
  Scheduled path: Every N hours → DB query → prompt → LLM → daily summary
"""

import asyncio
import concurrent.futures
import signal
from pathlib import Path

import structlog

from src.config import Config
from src.db.client import DBClient
from src.health import HealthReporter
from src.llm.engine import LLMEngine
from src.llm.templates import TemplateLoader
from src.mqtt.publisher import MQTTPublisher
from src.mqtt.subscriber import MQTTSubscriber, TriggerEvent
from src.report_builder import ReportBuilder

logger = structlog.get_logger("llm-analyzer")


class LLMAnalyzer:
    def __init__(self, config_path: str = "config.yaml"):
        self.cfg = Config.from_yaml(config_path)
        base_dir = Path(__file__).resolve().parent.parent  # services/llm-analyzer/

        # Model
        model_path = self.cfg.model.path
        if not Path(model_path).is_absolute():
            model_path = str(base_dir / model_path)
        self.llm = LLMEngine(
            model_path=model_path,
            n_ctx=self.cfg.model.context_length,
            n_threads=self.cfg.model.n_threads,
            temperature=self.cfg.model.temperature,
            top_p=self.cfg.model.top_p,
        )

        # Infrastructure
        self.db = DBClient(self.cfg.db)
        self.templates = TemplateLoader(str(base_dir / self.cfg.prompts_dir))
        self.mqtt_sub = MQTTSubscriber(self.cfg.mqtt, self.cfg.dedup)
        self.mqtt_pub = MQTTPublisher(self.cfg.mqtt)
        self.health = HealthReporter(self.cfg.mqtt, self.cfg.site_id)
        self.report_builder = ReportBuilder(
            self.cfg, self.db, self.llm, self.templates,
            self.mqtt_pub, self.health,
        )

        self._running = False
        self._trigger_queue: asyncio.Queue[TriggerEvent] = asyncio.Queue(maxsize=64)

    async def start(self):
        logger.info("llm-analyzer starting", site_id=self.cfg.site_id)

        # Load model (blocks briefly, ~2-5s for 1GB model)
        self.llm.load()
        self.health.model_loaded = True
        logger.info("model ready", name=self.llm.model_name,
                    version=self.llm.model_version)

        # Connect infrastructure
        self.db.connect()
        self.mqtt_sub.set_trigger_callback(self._on_trigger)
        self.mqtt_sub.connect()
        self.mqtt_pub.connect()
        self.health.connect()

        self._running = True
        tasks = [
            asyncio.create_task(self._trigger_consumer()),
            asyncio.create_task(self._daily_scheduler()),
            asyncio.create_task(self._health_loop()),
        ]
        logger.info("llm-analyzer running",
                    daily_interval_h=self.cfg.schedule.daily_summary_interval_h)

        loop = asyncio.get_event_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, self._shutdown)
            except NotImplementedError:
                pass

        try:
            await asyncio.gather(*tasks)
        except asyncio.CancelledError:
            pass

    def _shutdown(self):
        logger.info("shutting down")
        self._running = False
        self.llm.unload()
        self.mqtt_sub.disconnect()
        self.mqtt_pub.disconnect()
        self.health.disconnect()
        self.db.close()

    def _on_trigger(self, event: TriggerEvent):
        try:
            self._trigger_queue.put_nowait(event)
        except asyncio.QueueFull:
            logger.debug("trigger queue full", device=event.device_id)

    async def _trigger_consumer(self):
        while self._running:
            try:
                event = await asyncio.wait_for(
                    self._trigger_queue.get(), timeout=1.0)
                await self.report_builder.build_alert_report(event)
            except asyncio.TimeoutError:
                continue
            except Exception:
                logger.exception("trigger handler error")
                self.health.errors += 1

    async def _daily_scheduler(self):
        interval_h = self.cfg.schedule.daily_summary_interval_h
        if interval_h <= 0:
            logger.info("daily summary disabled")
            while self._running:
                await asyncio.sleep(60)
            return

        interval_s = interval_h * 3600
        logger.info("daily scheduler started", interval_h=interval_h)

        # Wait for the first interval before generating
        while self._running:
            await asyncio.sleep(interval_s)
            if not self._running:
                break
            try:
                await self.report_builder.build_daily_summary()
            except Exception:
                logger.exception("daily summary error")
                self.health.errors += 1

    async def _health_loop(self):
        interval = self.cfg.schedule.health_report_interval_s
        while self._running:
            await asyncio.sleep(interval)
            try:
                self.health.report()
            except Exception:
                logger.exception("health report error")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="EdgeVib LLM Analyzer")
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

    analyzer = LLMAnalyzer(args.config)
    try:
        asyncio.run(analyzer.start())
    except KeyboardInterrupt:
        analyzer._shutdown()


if __name__ == "__main__":
    main()
