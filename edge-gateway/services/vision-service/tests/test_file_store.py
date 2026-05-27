import os
import shutil
from datetime import datetime, timedelta

import pytest

from src.storage.file_store import (
    build_file_path, ensure_dir, write_jpeg, get_file_size,
    rotate_expired_files,
)


def test_build_file_path_baseline():
    from pathlib import Path
    path = build_file_path(
        "/base", "factory1", "motor01", "baseline",
        datetime(2026, 5, 27, 14, 30, 5),
    )
    assert Path(path) == Path("/base/factory1/motor01/2026-05-27/baseline_143005.jpg")


def test_build_file_path_event_with_severity():
    from pathlib import Path
    path = build_file_path(
        "/base", "factory1", "motor01", "event",
        datetime(2026, 5, 27, 14, 30, 5), severity="CRITICAL",
    )
    assert Path(path) == Path("/base/factory1/motor01/2026-05-27/event_143005_CRITICAL.jpg")


def test_build_file_path_event_no_severity():
    from pathlib import Path
    path = build_file_path(
        "/base", "factory1", "motor01", "event",
        datetime(2026, 5, 27, 14, 30, 5),
    )
    assert Path(path) == Path("/base/factory1/motor01/2026-05-27/event_143005.jpg")


def test_ensure_dir_creates_parents(tmp_path):
    file_path = os.path.join(tmp_path, "a", "b", "c.jpg")
    ensure_dir(file_path)
    assert os.path.isdir(os.path.dirname(file_path))


def test_write_jpeg_and_get_size(tmp_path, sample_jpeg_bytes):
    file_path = os.path.join(tmp_path, "test.jpg")
    write_jpeg(file_path, sample_jpeg_bytes)
    assert os.path.isfile(file_path)
    assert get_file_size(file_path) == len(sample_jpeg_bytes)


def test_write_jpeg_atomic(tmp_path, sample_jpeg_bytes):
    """Partial write via .tmp file should not leave a corrupt .jpg."""
    file_path = os.path.join(tmp_path, "test.jpg")
    write_jpeg(file_path, sample_jpeg_bytes)
    assert not os.path.exists(file_path + ".tmp")


def test_get_file_size_nonexistent():
    assert get_file_size("/nonexistent/file.jpg") == 0


def test_rotate_expired_files_basic(tmp_path):
    """Create date dirs at 1d, 8d, 15d old. With 7d retention, 8d+15d should be removed."""
    base = os.path.join(tmp_path, "vision")
    dev_dir = os.path.join(base, "factory1", "motor01")

    today = datetime.now()
    dates = [
        (today - timedelta(days=1), "1d"),
        (today - timedelta(days=8), "8d"),
        (today - timedelta(days=15), "15d"),
    ]
    for ts, _ in dates:
        date_dir = os.path.join(dev_dir, ts.strftime("%Y-%m-%d"))
        os.makedirs(date_dir, exist_ok=True)

    rotate_expired_files(base, 7)

    assert os.path.isdir(os.path.join(dev_dir, (today - timedelta(days=1)).strftime("%Y-%m-%d")))
    assert not os.path.isdir(os.path.join(dev_dir, (today - timedelta(days=8)).strftime("%Y-%m-%d")))
    assert not os.path.isdir(os.path.join(dev_dir, (today - timedelta(days=15)).strftime("%Y-%m-%d")))


def test_rotate_expired_files_zero_retention(tmp_path):
    """Zero retention should be a no-op."""
    base = os.path.join(tmp_path, "vision")
    os.makedirs(base, exist_ok=True)
    rotate_expired_files(base, 0)


def test_rotate_expired_files_nonexistent_dir():
    rotate_expired_files("/nonexistent/vision", 7)
