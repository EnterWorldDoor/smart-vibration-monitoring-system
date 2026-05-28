"""TimescaleDB client for audio-monitor.

Batch-buffered writes for high-frequency audio_features (~8 rows/s).
Best-effort: DB unavailability does NOT block audio capture.
Pattern mirrored from vision-service src/db/client.py.
"""

import json
import time
from datetime import datetime
from typing import Optional

import numpy as np
import psycopg2
import psycopg2.extras
import structlog

from src.config import TimescaleDBConfig
from src.audio.processor import AudioFrame

logger = structlog.get_logger(__name__)


class AudioDBClient:
    """Batch-accumulating TimescaleDB writer for audio features + anomalies."""

    def __init__(self, cfg: TimescaleDBConfig):
        self.cfg = cfg
        self._conn: Optional[psycopg2.extensions.connection] = None

        # Batch buffer for audio_features rows
        self._batch: list[tuple] = []
        self._batch_max_size: int = 15      # flush every ~15 frames
        self._batch_max_age_s: float = 1.0  # flush every 1 s max
        self._last_flush: float = 0.0

    # --- Connection lifecycle ------------------------------------------------

    def connect(self) -> bool:
        try:
            self._conn = psycopg2.connect(
                host=self.cfg.host, port=self.cfg.port,
                user=self.cfg.user, password=self.cfg.password,
                dbname=self.cfg.dbname, sslmode=self.cfg.sslmode,
            )
            self._conn.autocommit = False
            logger.info("db connected", host=self.cfg.host)
            return True
        except psycopg2.Error:
            logger.warning("db connect failed, running without DB",
                           host=self.cfg.host)
            self._conn = None
            return False

    def close(self) -> None:
        if self._conn:
            try:
                self._conn.close()
            except psycopg2.Error:
                pass
            self._conn = None

    def is_connected(self) -> bool:
        if self._conn is None:
            return False
        try:
            with self._conn.cursor() as cur:
                cur.execute("SELECT 1")
            return True
        except psycopg2.Error:
            return False

    # --- Feature batch writes ------------------------------------------------

    def buffer_feature(self, ts: datetime, site_id: str,
                        device_id: str, frame: AudioFrame) -> None:
        """Append one frame to in-memory batch.  O(1).  Never blocks."""
        spec = np.nan_to_num(frame.spectrum_128, nan=0.0, posinf=0.0, neginf=0.0)
        spec_json = json.dumps(spec.tolist() if len(spec) > 0 else [])
        self._batch.append((
            ts, site_id, device_id,
            float(frame.rms_energy),
            float(frame.spectral_centroid_hz),
            float(frame.spectral_kurtosis),
            float(frame.hf_lf_ratio),
            float(frame.dominant_freq_hz),
            float(frame.dominant_amp_db),
            spec_json,
        ))

    def should_flush(self) -> bool:
        if len(self._batch) >= self._batch_max_size:
            return True
        if self._batch and (time.time() - self._last_flush) >= self._batch_max_age_s:
            return True
        return False

    def flush_features(self) -> int:
        """INSERT all buffered rows.  Discard on failure.  Returns rows written."""
        if not self._batch or self._conn is None:
            self._batch.clear()
            return 0

        sql = (
            "INSERT INTO audio_features "
            "(time, site_id, device_id, rms_energy, spectral_centroid_hz, "
            " spectral_kurtosis, hf_lf_ratio, dominant_freq_hz, dominant_amp_db, "
            " feature_vector) "
            "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)"
        )
        n = len(self._batch)
        try:
            with self._conn.cursor() as cur:
                cur.executemany(sql, self._batch)
            self._conn.commit()
        except psycopg2.Error:
            logger.warning("db feature batch insert failed", batch_size=n)
            try:
                self._conn.rollback()
            except psycopg2.Error:
                pass
            n = 0
        finally:
            self._batch.clear()
            self._last_flush = time.time()

        return n

    # --- Anomaly record -----------------------------------------------------

    def insert_anomaly(
        self,
        ts: datetime, site_id: str, device_id: str,
        severity: str, trigger_reason: str,
        rms_energy: float, baseline_rms: float,
        sigma_level: float, wav_path: str,
        duration_ms: int, metadata: dict,
    ) -> bool:
        """Insert a single anomaly event.  Best-effort."""
        if self._conn is None:
            return False

        sql = (
            "INSERT INTO audio_anomalies "
            "(time, site_id, device_id, severity, trigger_reason, "
            " rms_energy, baseline_rms, sigma_level, wav_path, "
            " duration_ms, metadata) "
            "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)"
        )
        try:
            with self._conn.cursor() as cur:
                cur.execute(sql, (
                    ts, site_id, device_id, severity, trigger_reason,
                    rms_energy, baseline_rms, sigma_level, wav_path,
                    duration_ms, json.dumps(metadata),
                ))
            self._conn.commit()
            return True
        except psycopg2.Error:
            logger.warning("db anomaly insert failed",
                           device_id=device_id, severity=severity)
            try:
                self._conn.rollback()
            except psycopg2.Error:
                pass
            return False
