"""Filesystem JPEG storage with automatic rotation/cleanup."""

import os
import shutil
import structlog
from datetime import datetime, timedelta
from pathlib import Path

from src.config import CaptureConfig, DeviceConfig

logger = structlog.get_logger(__name__)

DATE_FMT = "%Y-%m-%d"
TIME_FMT = "%H%M%S"


def build_file_path(
    base_path: str,
    site_id: str,
    device_id: str,
    capture_type: str,
    timestamp: datetime,
    severity: str = "",
) -> str:
    """Build canonical file path for a vision capture.

    Pattern: {base}/{site_id}/{device_id}/{YYYY-MM-DD}/{type}_{HHMMSS}[_{severity}].jpg
    """
    date_str = timestamp.strftime(DATE_FMT)
    time_str = timestamp.strftime(TIME_FMT)
    if severity:
        filename = f"{capture_type}_{time_str}_{severity}.jpg"
    else:
        filename = f"{capture_type}_{time_str}.jpg"
    return str(Path(base_path) / site_id / device_id / date_str / filename)


def ensure_dir(file_path: str):
    Path(file_path).parent.mkdir(parents=True, exist_ok=True)


def write_jpeg(file_path: str, jpeg_bytes: bytes):
    """Write JPEG bytes to disk atomically via temp file + rename."""
    tmp_path = file_path + ".tmp"
    with open(tmp_path, "wb") as f:
        f.write(jpeg_bytes)
    os.replace(tmp_path, file_path)


def get_file_size(file_path: str) -> int:
    try:
        return os.path.getsize(file_path)
    except OSError:
        return 0


def rotate_expired_files(base_path: str, retention_days: int):
    """Delete date directories older than retention_days.

    Walks {base}/{site}/{device}/{date_dir}/ and removes date directories
    whose name is lexicographically <= cutoff ISO date string.
    """
    if retention_days <= 0:
        return
    cutoff_date = datetime.now() - timedelta(days=retention_days)
    cutoff_str = cutoff_date.strftime(DATE_FMT)

    base = Path(base_path)
    if not base.exists():
        return

    deleted = 0
    for site_dir in base.iterdir():
        if not site_dir.is_dir():
            continue
        for dev_dir in site_dir.iterdir():
            if not dev_dir.is_dir():
                continue
            for date_dir in dev_dir.iterdir():
                if not date_dir.is_dir():
                    continue
                if date_dir.name <= cutoff_str:
                    shutil.rmtree(date_dir)
                    deleted += 1
                    logger.info("rotated", path=str(date_dir),
                                retention_days=retention_days)
    if deleted > 0:
        logger.info("rotation complete", deleted_dirs=deleted)
