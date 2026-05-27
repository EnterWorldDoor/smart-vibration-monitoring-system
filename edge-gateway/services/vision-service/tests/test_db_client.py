from datetime import datetime, timezone
from unittest.mock import patch, MagicMock

import pytest

from src.db.client import VisonDBClient
from src.config import TimescaleDBConfig


@pytest.fixture
def db_cfg():
    return TimescaleDBConfig()


@pytest.fixture
def mock_psycopg2():
    with patch("src.db.client.psycopg2") as m:
        conn = MagicMock()
        m.connect.return_value = conn
        yield m


def test_connect_success(db_cfg, mock_psycopg2):
    client = VisonDBClient(db_cfg)
    client.connect()
    assert client._conn is not None
    client._conn.autocommit = False  # Verify autocommit was set


def test_connect_failure_no_raise(db_cfg, mock_psycopg2):
    mock_psycopg2.connect.side_effect = RuntimeError("connection refused")
    client = VisonDBClient(db_cfg)
    client.connect()
    assert client._conn is None


def test_ping_returns_true(db_cfg, mock_psycopg2):
    client = VisonDBClient(db_cfg)
    client.connect()
    assert client.ping() is True


def test_ping_returns_false_when_disconnected(db_cfg):
    client = VisonDBClient(db_cfg)
    assert client.ping() is False


def test_insert_capture_success(db_cfg, mock_psycopg2):
    client = VisonDBClient(db_cfg)
    client.connect()

    client.insert_capture(
        timestamp=datetime(2026, 5, 27, 14, 30, 0, tzinfo=timezone.utc),
        site_id="factory1",
        device_id="motor01",
        capture_type="baseline",
        trigger_src="timer",
        resolution="640x480",
        file_path="/opt/vision/factory1/motor01/2026-05-27/baseline_143000.jpg",
        file_size_bytes=12345,
    )

    conn = mock_psycopg2.connect.return_value
    conn.cursor.return_value.__enter__.return_value.execute.assert_called_once()
    conn.commit.assert_called_once()


def test_insert_capture_noop_when_disconnected(db_cfg):
    client = VisonDBClient(db_cfg)
    client.insert_capture(
        timestamp=datetime.now(timezone.utc),
        site_id="factory1", device_id="motor01",
        capture_type="baseline", trigger_src="timer",
        resolution="640x480", file_path="/tmp/x.jpg",
        file_size_bytes=0,
    )


def test_insert_failure_rollback(db_cfg, mock_psycopg2):
    conn = mock_psycopg2.connect.return_value
    conn.cursor.return_value.__enter__.return_value.execute.side_effect = RuntimeError("insert failed")

    client = VisonDBClient(db_cfg)
    client.connect()

    client.insert_capture(
        timestamp=datetime.now(timezone.utc),
        site_id="factory1", device_id="motor01",
        capture_type="baseline", trigger_src="timer",
        resolution="640x480", file_path="/tmp/x.jpg",
        file_size_bytes=0,
    )

    conn.rollback.assert_called_once()


def test_close(db_cfg, mock_psycopg2):
    client = VisonDBClient(db_cfg)
    client.connect()
    client.close()
    assert client._conn is None
    mock_psycopg2.connect.return_value.close.assert_called_once()
