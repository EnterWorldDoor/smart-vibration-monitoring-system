"""Test AudioDBClient (mock psycopg2)."""

from datetime import datetime, timezone
from unittest.mock import MagicMock, patch, ANY

import pytest

from src.audio.processor import AudioFrame
from src.db.client import AudioDBClient
from src.config import TimescaleDBConfig

import numpy as np


@pytest.fixture
def db_cfg():
    return TimescaleDBConfig(
        host="testhost", port=5432, dbname="testdb",
        user="testuser", password="testpass",
    )


@pytest.fixture
def db(db_cfg):
    return AudioDBClient(db_cfg)


@pytest.fixture
def sample_frame():
    return AudioFrame(
        timestamp=100.0,
        rms_energy=0.01,
        spectral_centroid_hz=1500.0,
        spectral_kurtosis=2.8,
        hf_lf_ratio=0.1,
        dominant_freq_hz=440.0,
        dominant_amp_db=-20.0,
        spectrum_128=np.random.randn(128).astype(np.float32),
        raw_audio=np.zeros(2048, dtype=np.int16),
    )


class TestBatchBuffer:
    def test_buffer_feature_appends(self, db, sample_frame):
        ts = datetime.now(timezone.utc)
        db.buffer_feature(ts, "factory1", "audio01", sample_frame)
        assert len(db._batch) == 1

    def test_buffer_many(self, db, sample_frame):
        ts = datetime.now(timezone.utc)
        for _ in range(20):
            db.buffer_feature(ts, "site", "dev", sample_frame)
        assert len(db._batch) == 20

    def test_should_flush_size(self, db, sample_frame):
        ts = datetime.now(timezone.utc)
        for _ in range(15):
            db.buffer_feature(ts, "site", "dev", sample_frame)
        assert db.should_flush() is True

    def test_should_flush_false_when_empty(self, db):
        assert db.should_flush() is False

    def test_should_flush_false_when_small(self, db, sample_frame):
        ts = datetime.now(timezone.utc)
        for _ in range(5):
            db.buffer_feature(ts, "site", "dev", sample_frame)
        db._last_flush = __import__("time").time()  # just now
        assert db.should_flush() is False


class TestFlush:
    def test_flush_clears_buffer(self, db, sample_frame):
        ts = datetime.now(timezone.utc)
        for _ in range(10):
            db.buffer_feature(ts, "site", "dev", sample_frame)

        # Without a real connection, flush should discard
        n = db.flush_features()
        assert n == 0  # no connection
        assert len(db._batch) == 0

    @patch("src.db.client.psycopg2")
    def test_flush_with_connection(self, mock_psycopg2, db_cfg,
                                     sample_frame):
        mock_conn = MagicMock()
        mock_psycopg2.connect.return_value = mock_conn

        db = AudioDBClient(db_cfg)
        db.connect()
        ts = datetime.now(timezone.utc)

        for _ in range(10):
            db.buffer_feature(ts, "factory1", "audio01", sample_frame)

        db.flush_features()
        # executemany should have been called
        mock_cursor = mock_conn.cursor.return_value.__enter__.return_value
        mock_cursor.executemany.assert_called_once()
        mock_conn.commit.assert_called_once()
        assert len(db._batch) == 0

    @patch("src.db.client.psycopg2")
    def test_flush_rollback_on_error(self, mock_psycopg2, db_cfg,
                                      sample_frame):
        mock_conn = MagicMock()
        mock_cursor = mock_conn.cursor.return_value.__enter__.return_value
        mock_cursor.executemany.side_effect = \
            mock_psycopg2.Error("simulated error")
        mock_psycopg2.connect.return_value = mock_conn

        db = AudioDBClient(db_cfg)
        db.connect()
        ts = datetime.now(timezone.utc)

        for _ in range(10):
            db.buffer_feature(ts, "factory1", "audio01", sample_frame)

        db.flush_features()
        mock_conn.rollback.assert_called_once()
        assert len(db._batch) == 0  # buffer cleared on failure


class TestAnomalyInsert:
    @patch("src.db.client.psycopg2")
    def test_insert_anomaly_success(self, mock_psycopg2, db_cfg):
        mock_conn = MagicMock()
        mock_psycopg2.connect.return_value = mock_conn

        db = AudioDBClient(db_cfg)
        db.connect()
        ts = datetime.now(timezone.utc)

        ok = db.insert_anomaly(
            ts=ts, site_id="fab1", device_id="dev1",
            severity="warning", trigger_reason="test",
            rms_energy=0.05, baseline_rms=0.01,
            sigma_level=4.0, wav_path="/tmp/test.wav",
            duration_ms=5000, metadata={"kurtosis": 7.0},
        )
        assert ok is True

    def test_insert_anomaly_no_connection(self, db):
        ok = db.insert_anomaly(
            ts=datetime.now(timezone.utc),
            site_id="fab1", device_id="dev1",
            severity="warning", trigger_reason="test",
            rms_energy=0.0, baseline_rms=0.0,
            sigma_level=0.0, wav_path="", duration_ms=0, metadata={},
        )
        assert ok is False


class TestConnection:
    @patch("src.db.client.psycopg2")
    def test_connect_success(self, mock_psycopg2, db_cfg):
        mock_conn = MagicMock()
        mock_psycopg2.connect.return_value = mock_conn
        db = AudioDBClient(db_cfg)
        assert db.connect() is True
        assert db.is_connected() is True

    @patch("src.db.client.psycopg2")
    def test_connect_failure(self, mock_psycopg2, db_cfg):
        mock_psycopg2.connect.side_effect = mock_psycopg2.Error(
            "connection refused"
        )
        db = AudioDBClient(db_cfg)
        assert db.connect() is False
        assert db.is_connected() is False

    @patch("src.db.client.psycopg2")
    def test_close(self, mock_psycopg2, db_cfg):
        mock_conn = MagicMock()
        mock_psycopg2.connect.return_value = mock_conn
        db = AudioDBClient(db_cfg)
        db.connect()
        db.close()
        mock_conn.close.assert_called_once()
