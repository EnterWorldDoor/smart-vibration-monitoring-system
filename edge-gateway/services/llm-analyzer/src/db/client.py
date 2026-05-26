"""TimescaleDB client for llm-analyzer — read ai_reports/vibration, write llm_reports."""

import structlog
from datetime import datetime, timezone
from typing import Optional

import psycopg2
import psycopg2.extras

from src.config import DBConfig

logger = structlog.get_logger(__name__)

VIBRATION_VIEW = "vibration_view"


class DBClient:
    def __init__(self, cfg: DBConfig):
        self.cfg = cfg
        self._conn = None

    def connect(self):
        self._conn = psycopg2.connect(
            host=self.cfg.host, port=self.cfg.port,
            user=self.cfg.user, password=self.cfg.password,
            dbname=self.cfg.dbname, sslmode=self.cfg.sslmode,
        )
        self._conn.autocommit = False
        logger.info("db connected", host=self.cfg.host, db=self.cfg.dbname)

    def close(self):
        if self._conn:
            self._conn.close()
            self._conn = None

    def ping(self) -> bool:
        try:
            with self._conn.cursor() as cur:
                cur.execute("SELECT 1")
            return True
        except Exception:
            return False

    def query_recent_ai_reports(self, site_id: str, device_id: str,
                                minutes: int = 10, limit: int = 5):
        """Query recent ai_reports for a device to build LLM context."""
        sql = """
            SELECT time, report_type, device_id, severity, payload,
                   model_name, model_version, anomaly_score, health_score,
                   inference_time_ms, details
            FROM ai_reports
            WHERE site_id = %s AND device_id = %s
              AND time > NOW() - INTERVAL '%s minutes'
            ORDER BY time DESC
            LIMIT %s
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, device_id, minutes, limit))
            rows = cur.fetchall()
        if not rows:
            return []
        rows.reverse()
        return rows

    def query_recent_vibration(self, site_id: str, device_id: str,
                               minutes: int = 10, limit: int = 30):
        """Query recent vibration data for context enrichment."""
        sql = f"""
            SELECT time, rms_x, rms_y, rms_z, overall_rms,
                   peak_frequency_hz, peak_amplitude_g
            FROM {VIBRATION_VIEW}
            WHERE site_id = %s AND device_id = %s
              AND time > NOW() - INTERVAL '%s minutes'
            ORDER BY time DESC
            LIMIT %s
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, device_id, minutes, limit))
            rows = cur.fetchall()
        if not rows:
            return []
        rows.reverse()
        return rows

    def query_ai_reports_in_window(self, site_id: str, hours: int = 8):
        """Query all ai_reports in the past N hours for daily summary."""
        sql = """
            SELECT time, report_type, device_id, severity, payload,
                   anomaly_score, health_score, details
            FROM ai_reports
            WHERE site_id = %s
              AND time > NOW() - INTERVAL '%s hours'
            ORDER BY time DESC
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, hours))
            return cur.fetchall()

    def insert_llm_report(
        self,
        time: datetime,
        site_id: str,
        device_id: str,
        report_type: str,
        severity: str,
        title: str,
        summary: str,
        analysis: str,
        advice: str,
        raw_output: str,
        model_name: str,
        model_version: str,
        tokens_used: int,
        generation_time_ms: float,
        trigger_reason: str,
    ):
        sql = """
            INSERT INTO llm_reports (time, site_id, device_id, report_type,
                severity, title, summary, analysis, advice, raw_output,
                model_name, model_version, tokens_used, generation_time_ms,
                trigger_reason)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
        """
        with self._conn.cursor() as cur:
            cur.execute(sql, (
                time, site_id, device_id, report_type,
                severity, title, summary, analysis, advice, raw_output,
                model_name, model_version, tokens_used, generation_time_ms,
                trigger_reason,
            ))
        self._conn.commit()
        logger.info("llm_report inserted", device_id=device_id,
                    report_type=report_type, severity=severity)
