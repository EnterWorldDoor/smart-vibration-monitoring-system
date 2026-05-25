"""TimescaleDB client for inference-engine — query VIEWs and write reports."""

import json
import structlog
from typing import Optional
from datetime import datetime, timezone

import psycopg2
import psycopg2.extras

from src.config import DBConfig

logger = structlog.get_logger(__name__)

FEATURE_VECTOR_VIEW = "feature_vector_view"
VIBRATION_VIEW = "vibration_view"
AI_DIAGNOSIS_VIEW = "ai_diagnosis_view"
DUAL_CHANNEL_VIEW = "dual_channel_view"


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

    def query_recent_features(self, site_id: str, device_id: str, limit: int = 30):
        """Query recent 24-dim feature vectors for a device from feature_vector_view."""
        sql = f"""
            SELECT time, feat_rms_x, feat_rms_y, feat_rms_z, feat_overall_rms,
                   feat_peak_freq_x, feat_peak_amp_x, feat_skewness_x,
                   feat_kurtosis_x, feat_crest_factor_x,
                   feat_band_energy_x_0, feat_band_energy_x_1,
                   feat_band_energy_x_2, feat_band_energy_x_3,
                   feat_band_energy_x_4, feat_band_energy_x_5,
                   feat_band_energy_x_6, feat_band_energy_x_7,
                   feat_peak_freq_y, feat_peak_amp_y, feat_crest_factor_y,
                   feat_peak_freq_z, feat_peak_amp_z, feat_crest_factor_z,
                   feat_temperature_c
            FROM {FEATURE_VECTOR_VIEW}
            WHERE site_id = %s AND device_id = %s
            ORDER BY time DESC
            LIMIT %s
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, device_id, limit))
            rows = cur.fetchall()
        if not rows:
            return []
        rows.reverse()  # chronological order
        return rows

    def query_recent_vibration(self, site_id: str, device_id: str, limit: int = 30):
        """Query recent vibration data from vibration_view (fallback when features unavailable)."""
        sql = f"""
            SELECT time, rms_x, rms_y, rms_z, overall_rms,
                   peak_frequency_hz, peak_amplitude_g
            FROM {VIBRATION_VIEW}
            WHERE site_id = %s AND device_id = %s
              AND time > NOW() - INTERVAL '10 minutes'
            ORDER BY time DESC
            LIMIT %s
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, device_id, limit))
            rows = cur.fetchall()
        if not rows:
            return []
        rows.reverse()
        return rows

    def query_ai_diagnosis(self, site_id: str, device_id: str, limit: int = 10):
        """Query recent AI classification results."""
        sql = f"""
            SELECT time, ai_class_id, ai_class_name, ai_confidence, ai_cascade_source
            FROM {AI_DIAGNOSIS_VIEW}
            WHERE site_id = %s AND device_id = %s
            ORDER BY time DESC
            LIMIT %s
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, device_id, limit))
            rows = cur.fetchall()
        if not rows:
            return []
        rows.reverse()
        return rows

    def query_dual_channel(self, site_id: str, device_id: str, limit: int = 30):
        """Query DE/NDE dual channel comparison data."""
        sql = f"""
            SELECT time, rms_ratio, spectral_similarity, phase_coherence,
                   nde_online, nde_errors
            FROM {DUAL_CHANNEL_VIEW}
            WHERE site_id = %s AND device_id = %s
            ORDER BY time DESC
            LIMIT %s
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, device_id, limit))
            rows = cur.fetchall()
        if not rows:
            return []
        rows.reverse()
        return rows

    def query_distinct_devices(self, site_id: str):
        """Get all distinct device_ids that have sent data recently."""
        sql = """
            SELECT DISTINCT device_type, device_id
            FROM sensor_data
            WHERE site_id = %s
              AND source_path = 'mqtt'
              AND time > NOW() - INTERVAL '5 minutes'
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id,))
            return cur.fetchall()

    def query_latest_payload_json(self, site_id: str, device_id: str):
        """Get the most recent raw JSONB payload for a device — used for MQTT trigger fallback."""
        sql = """
            SELECT payload, time
            FROM sensor_data
            WHERE site_id = %s AND device_id = %s AND source_path = 'mqtt'
            ORDER BY time DESC
            LIMIT 1
        """
        with self._conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(sql, (site_id, device_id))
            row = cur.fetchone()
        return row

    def insert_report(
        self,
        time: datetime,
        site_id: str,
        report_type: str,
        device_id: str,
        severity: str,
        payload: dict,
        model_name: Optional[str] = None,
        model_version: Optional[str] = None,
        anomaly_score: Optional[float] = None,
        health_score: Optional[float] = None,
        inference_time_ms: Optional[float] = None,
        details: Optional[dict] = None,
    ):
        sql = """
            INSERT INTO ai_reports (time, site_id, report_type, device_id,
                severity, payload, model_name, model_version,
                anomaly_score, health_score, inference_time_ms, details)
            VALUES (%s, %s, %s, %s, %s, %s::jsonb, %s, %s, %s, %s, %s, %s::jsonb)
        """
        with self._conn.cursor() as cur:
            cur.execute(sql, (
                time, site_id, report_type, device_id,
                severity, json.dumps(payload), model_name, model_version,
                anomaly_score, health_score, inference_time_ms,
                json.dumps(details) if details else None,
            ))
        self._conn.commit()
