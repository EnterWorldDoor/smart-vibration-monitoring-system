"""TimescaleDB client for vision-service — best-effort metadata writes."""

import structlog
from datetime import datetime
from typing import Optional

import psycopg2
import psycopg2.extras

from src.config import TimescaleDBConfig

logger = structlog.get_logger(__name__)


class VisonDBClient:
    """Best-effort writer for vision_captures metadata table."""

    def __init__(self, cfg: TimescaleDBConfig):
        self.cfg = cfg
        self._conn: Optional[psycopg2.extensions.connection] = None

    def connect(self):
        try:
            self._conn = psycopg2.connect(
                host=self.cfg.host, port=self.cfg.port,
                user=self.cfg.user, password=self.cfg.password,
                dbname=self.cfg.dbname, sslmode=self.cfg.sslmode,
            )
            self._conn.autocommit = False
            logger.info("db connected", host=self.cfg.host, db=self.cfg.dbname)
        except Exception:
            logger.warning("db connect failed -- metadata writes disabled",
                           host=self.cfg.host)

    def close(self):
        if self._conn:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None

    def ping(self) -> bool:
        if self._conn is None:
            return False
        try:
            with self._conn.cursor() as cur:
                cur.execute("SELECT 1")
            return True
        except Exception:
            return False

    def insert_capture(
        self,
        timestamp: datetime,
        site_id: str,
        device_id: str,
        capture_type: str,
        trigger_src: str,
        resolution: str,
        file_path: str,
        file_size_bytes: int,
    ):
        if self._conn is None:
            return
        sql = """
            INSERT INTO vision_captures
                (time, site_id, device_id, capture_type, trigger_src,
                 resolution, file_path, file_size_bytes)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
        """
        try:
            with self._conn.cursor() as cur:
                cur.execute(sql, (
                    timestamp, site_id, device_id, capture_type, trigger_src,
                    resolution, file_path, file_size_bytes,
                ))
            self._conn.commit()
        except Exception:
            logger.warning("db insert failed", file_path=file_path,
                           capture_type=capture_type)
            try:
                self._conn.rollback()
            except Exception:
                pass
