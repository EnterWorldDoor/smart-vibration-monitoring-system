"""Report builder — data context assembly → LLM inference → report parsing.

Core orchestration layer:
  1. Query TimescaleDB for context (ai_reports, vibration_view)
  2. Build prompt from template + data
  3. Call LLM via run_in_executor
  4. Parse output into structured report
  5. Write to DB + publish to MQTT
"""

import asyncio
import concurrent.futures
import re
import time
from datetime import datetime, timezone
from typing import Optional

import structlog

from src.config import Config
from src.db.client import DBClient
from src.llm.engine import LLMEngine
from src.llm.templates import TemplateLoader
from src.health import HealthReporter
from src.mqtt.publisher import MQTTPublisher
from src.mqtt.subscriber import TriggerEvent

logger = structlog.get_logger(__name__)

# Regex patterns for parsing LLM four-section output
TITLE_PATTERN = re.compile(r'【标题】\s*(.+?)(?:\n|$)')
STATUS_PATTERN = re.compile(r'【当前状态】\s*(.+?)(?=【异常分析】|【维护建议】|$)',
                            re.DOTALL)
ANALYSIS_PATTERN = re.compile(r'【异常分析】\s*(.+?)(?=【维护建议】|$)',
                              re.DOTALL)
ADVICE_PATTERN = re.compile(r'【维护建议】\s*(.+?)$', re.DOTALL)


def parse_report_sections(raw_text: str) -> dict:
    """Parse LLM output into title/summary/analysis/advice.

    Falls back gracefully: if structured format not found, uses full text as summary.
    """
    title = ""
    summary = ""
    analysis = ""
    advice = ""

    m = TITLE_PATTERN.search(raw_text)
    if m:
        title = m.group(1).strip()

    m = STATUS_PATTERN.search(raw_text)
    if m:
        summary = m.group(1).strip()

    m = ANALYSIS_PATTERN.search(raw_text)
    if m:
        analysis = m.group(1).strip()

    m = ADVICE_PATTERN.search(raw_text)
    if m:
        advice = m.group(1).strip()

    if not any([title, summary, analysis, advice]):
        summary = raw_text[:500]

    return {
        "title": title,
        "summary": summary,
        "analysis": analysis,
        "advice": advice,
    }


class ReportBuilder:
    def __init__(self, cfg: Config, db: DBClient, llm: LLMEngine,
                 templates: TemplateLoader, mqtt_pub: MQTTPublisher,
                 health: HealthReporter):
        self.cfg = cfg
        self.db = db
        self.llm = llm
        self.templates = templates
        self.mqtt_pub = mqtt_pub
        self.health = health
        self._executor = concurrent.futures.ThreadPoolExecutor(max_workers=1)

    async def build_alert_report(self, event: TriggerEvent):
        """Handle an MQTT trigger event: query context → LLM → write results."""
        logger.info("building alert report", device_id=event.device_id,
                    severity=event.severity, reason=event.trigger_reason)
        loop = asyncio.get_event_loop()

        # 1. Query context from DB
        ai_reports = await loop.run_in_executor(
            self._executor,
            self.db.query_recent_ai_reports,
            event.site_id, event.device_id, 10, 5,
        )
        vibration_rows = await loop.run_in_executor(
            self._executor,
            self.db.query_recent_vibration,
            event.site_id, event.device_id, 10, 30,
        )

        # 2. Build data context
        latest_report = ai_reports[-1] if ai_reports else {}
        health_score = latest_report.get("health_score", "N/A")
        anomaly_score = latest_report.get("anomaly_score", "N/A")

        details = latest_report.get("details") or {}
        rms_slope = details.get("rms_slope", 0)
        freq_drift = details.get("freq_drift_std", 0)
        crest_slope = details.get("crest_factor_slope", 0)

        ai_warnings = details.get("trend_warnings", [])
        warnings_text = "\n".join(f"- {w}" for w in ai_warnings) if ai_warnings else "无"

        current_rms = 0.0
        peak_freq = 0.0
        vib_lines = []
        for row in vibration_rows[-10:]:
            rms = row.get("overall_rms") or 0
            freq = row.get("peak_frequency_hz") or 0
            vib_lines.append(
                f"  {row['time']}: RMS={rms:.2f}mm/s, Freq={freq:.0f}Hz")
            current_rms = rms
            peak_freq = freq
        vibration_summary = "\n".join(vib_lines) if vib_lines else "无振动数据"

        device_info = f"{event.site_id} {event.device_id}"

        # 3. Build prompt & call LLM
        template = self.templates.load("alert_report")
        prompt_params = {
            "device_info": device_info,
            "severity": event.severity,
            "trigger_reason": event.trigger_reason,
            "health_score": health_score,
            "anomaly_score": anomaly_score,
            "current_rms": f"{current_rms:.2f}",
            "rms_slope": f"{rms_slope:.4f}",
            "peak_frequency_hz": f"{peak_freq:.1f}",
            "freq_drift_std": f"{freq_drift:.1f}",
            "crest_factor_slope": f"{crest_slope:.4f}",
            "vibration_summary": vibration_summary,
            "ai_warnings": warnings_text,
        }
        messages = template.build_messages(**prompt_params)

        t0 = time.perf_counter()
        try:
            result = await loop.run_in_executor(
                self._executor,
                self.llm.chat,
                messages,
                self.cfg.model.max_tokens,
            )
        except Exception:
            logger.exception("llm generation failed")
            self.health.errors += 1
            return

        gen_time = (time.perf_counter() - t0) * 1000
        self.health.total_generations += 1
        self.health.last_generation_time_ms = gen_time

        # 4. Parse output
        raw_text = result["text"]
        sections = parse_report_sections(raw_text)

        # 5. Write to DB
        now = datetime.now(timezone.utc)
        await loop.run_in_executor(self._executor, self.db.insert_llm_report,
            now, event.site_id, event.device_id, "alert_report",
            event.severity,
            sections["title"], sections["summary"],
            sections["analysis"], sections["advice"],
            raw_text,
            self.llm.model_name, self.llm.model_version,
            result["tokens_used"], gen_time,
            event.trigger_reason,
        )
        self.health.total_reports += 1

        # 6. Publish to MQTT
        await loop.run_in_executor(self._executor,
            self.mqtt_pub.publish_report,
            event.site_id, event.device_id, {
                "report_type": "alert_report",
                "severity": event.severity,
                "title": sections["title"],
                "summary": sections["summary"],
                "analysis": sections["analysis"],
                "advice": sections["advice"],
                "model_name": self.llm.model_name,
                "tokens_used": result["tokens_used"],
                "generation_time_ms": round(gen_time, 0),
                "timestamp_utc": now.isoformat(),
            })

    async def build_daily_summary(self):
        """Generate a daily summary report covering the configured time window."""
        window_h = self.cfg.schedule.daily_summary_interval_h
        if window_h <= 0:
            return

        logger.info("building daily summary", window_hours=window_h)
        loop = asyncio.get_event_loop()

        # 1. Query all ai_reports in the window
        rows = await loop.run_in_executor(
            self._executor,
            self.db.query_ai_reports_in_window,
            self.cfg.site_id, window_h,
        )

        if not rows:
            logger.info("no ai_reports in window, skipping daily summary")
            return

        # 2. Aggregate statistics
        total = len(rows)
        severity_counts = {"NORMAL": 0, "WARNING": 0, "CRITICAL": 0}
        device_stats: dict[str, dict] = {}

        for r in rows:
            sev = r.get("severity", "NORMAL") or "NORMAL"
            severity_counts[sev] = severity_counts.get(sev, 0) + 1
            did = r["device_id"]
            if did not in device_stats:
                device_stats[did] = {
                    "health_score": r.get("health_score"),
                    "anomaly_count": 0,
                    "status": "正常",
                }
            details = r.get("details") or {}
            if details.get("anomaly_detected"):
                device_stats[did]["anomaly_count"] += 1

        severity_text = ", ".join(
            f"{k} {v}次" for k, v in severity_counts.items() if v > 0)

        device_lines = []
        for did, stats in device_stats.items():
            hs = stats["health_score"] or 100
            if hs >= 80:
                status = "NORMAL"
            elif hs >= 50:
                status = "WARNING"
            else:
                status = "CRITICAL"
            device_lines.append(
                f"- {did}: 健康评分 {hs:.0f}/100 ({status}), "
                f"{stats['anomaly_count']}次异常检测"
            )
        device_summaries = "\n".join(device_lines) if device_lines else "无设备数据"

        # 3. Build prompt & call LLM
        template = self.templates.load("daily_summary")
        prompt_params = {
            "site_id": self.cfg.site_id,
            "window_hours": str(window_h),
            "report_count": str(total),
            "severity_distribution": severity_text,
            "device_summaries": device_summaries,
        }
        messages = template.build_messages(**prompt_params)

        t0 = time.perf_counter()
        try:
            result = await loop.run_in_executor(
                self._executor,
                self.llm.chat,
                messages,
                self.cfg.model.max_tokens,
            )
        except Exception:
            logger.exception("llm generation failed for daily summary")
            self.health.errors += 1
            return

        gen_time = (time.perf_counter() - t0) * 1000
        self.health.total_generations += 1
        self.health.last_generation_time_ms = gen_time

        # 4. Parse output
        raw_text = result["text"]
        sections = parse_report_sections(raw_text)

        # 5. Write to DB (device_id is empty for site-level summaries)
        now = datetime.now(timezone.utc)
        await loop.run_in_executor(self._executor, self.db.insert_llm_report,
            now, self.cfg.site_id, "", "daily_summary", "NORMAL",
            sections["title"], sections["summary"],
            sections["analysis"], sections["advice"],
            raw_text,
            self.llm.model_name, self.llm.model_version,
            result["tokens_used"], gen_time,
            "scheduled",
        )
        self.health.total_reports += 1

        # 6. Publish to MQTT (site-level topic)
        await loop.run_in_executor(self._executor,
            self.mqtt_pub.publish_report,
            self.cfg.site_id, "site", {
                "report_type": "daily_summary",
                "title": sections["title"],
                "summary": sections["summary"],
                "analysis": sections["analysis"],
                "advice": sections["advice"],
                "window_hours": window_h,
                "report_count": total,
                "severity_distribution": severity_text,
                "model_name": self.llm.model_name,
                "tokens_used": result["tokens_used"],
                "generation_time_ms": round(gen_time, 0),
                "timestamp_utc": now.isoformat(),
            })
